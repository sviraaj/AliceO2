// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "Framework/AODReaderHelpers.h"
#include "Framework/AnalysisDataModel.h"
#include "DataProcessingHelpers.h"
#include "Framework/RootTableBuilderHelpers.h"
#include "Framework/AlgorithmSpec.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/ControlService.h"
#include "Framework/DeviceSpec.h"
#include "Framework/RawDeviceService.h"
#include "Framework/DataSpecUtils.h"
#include "Framework/SourceInfoHeader.h"
#include "Framework/ChannelInfo.h"
#include "Framework/Logger.h"

#include <FairMQDevice.h>
#include <ROOT/RDataFrame.hxx>
#include <TFile.h>

#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/io/interfaces.h>
#include <arrow/table.h>
#include <arrow/util/key_value_metadata.h>

#include <thread>

namespace o2::framework::readers
{
namespace
{

// Input stream that just reads from stdin.
class FileStream : public arrow::io::InputStream
{
 public:
  FileStream(FILE* stream) : mStream(stream)
  {
    set_mode(arrow::io::FileMode::READ);
  }
  ~FileStream() override = default;

  // FIXME: handle return code
  arrow::Status Close() override { return arrow::Status::OK(); }
  bool closed() const override { return false; }

  arrow::Status Tell(int64_t* position) const override
  {
    *position = mPos;
    return arrow::Status::OK();
  }

  arrow::Status Read(int64_t nbytes, int64_t* bytes_read, void* out) override
  {
    auto count = fread(out, nbytes, 1, mStream);
    if (ferror(mStream) == 0) {
      *bytes_read = nbytes;
      mPos += nbytes;
    } else {
      *bytes_read = 0;
    }
    return arrow::Status::OK();
  }

  arrow::Status Read(int64_t nbytes, std::shared_ptr<arrow::Buffer>* out) override
  {
    std::shared_ptr<arrow::ResizableBuffer> buffer;
    ARROW_RETURN_NOT_OK(AllocateResizableBuffer(nbytes, &buffer));
    int64_t bytes_read;
    ARROW_RETURN_NOT_OK(Read(nbytes, &bytes_read, buffer->mutable_data()));
    ARROW_RETURN_NOT_OK(buffer->Resize(bytes_read, false));
    buffer->ZeroPadding();
    *out = buffer;
    return arrow::Status::OK();
  }

 private:
  FILE* mStream = nullptr;
  int64_t mPos = 0;
};

} // anonymous namespace

enum AODTypeMask : uint64_t {
  None = 0,
  Tracks = 1 << 0,
  TracksCov = 1 << 1,
  TracksExtra = 1 << 2,
  Calo = 1 << 3,
  Muon = 1 << 4,
  VZero = 1 << 5,
  Zdc = 1 << 6,
  Trigger = 1 << 7,
  Collisions = 1 << 8,
  Timeframe = 1 << 9,
  Unknown = 1 << 11
};

uint64_t getMask(header::DataDescription description)
{

  if (description == header::DataDescription{"TRACKPAR"}) {
    return AODTypeMask::Tracks;
  } else if (description == header::DataDescription{"TRACKPARCOV"}) {
    return AODTypeMask::TracksCov;
  } else if (description == header::DataDescription{"TRACKEXTRA"}) {
    return AODTypeMask::TracksExtra;
  } else if (description == header::DataDescription{"CALO"}) {
    return AODTypeMask::Calo;
  } else if (description == header::DataDescription{"MUON"}) {
    return AODTypeMask::Muon;
  } else if (description == header::DataDescription{"VZERO"}) {
    return AODTypeMask::VZero;
  } else if (description == header::DataDescription{"ZDC"}) {
    return AODTypeMask::Zdc;
  } else if (description == header::DataDescription{"TRIGGER"}) {
    return AODTypeMask::Trigger;
  } else if (description == header::DataDescription{"COLLISION"}) {
    return AODTypeMask::Collisions;
  } else if (description == header::DataDescription{"TIMEFRAME"}) {
    return AODTypeMask::Timeframe;
  } else {
    LOG(DEBUG) << "This is a tree of unknown type! " << description.str;
    return AODTypeMask::Unknown;
  }
}

uint64_t calculateReadMask(std::vector<OutputRoute> const& routes, header::DataOrigin const& origin)
{
  uint64_t readMask = None;
  for (auto& route : routes) {
    auto concrete = DataSpecUtils::asConcreteDataTypeMatcher(route.matcher);
    auto description = concrete.description;

    readMask |= getMask(description);
  }
  return readMask;
}

std::vector<OutputRoute> getListOfUnknown(std::vector<OutputRoute> const& routes)
{

  std::vector<OutputRoute> unknows;
  for (auto& route : routes) {
    auto concrete = DataSpecUtils::asConcreteDataTypeMatcher(route.matcher);

    if (getMask(concrete.description) == AODTypeMask::Unknown)
      unknows.push_back(route);
  }
  return unknows;
}

AlgorithmSpec AODReaderHelpers::run2ESDConverterCallback()
{
  auto callback = AlgorithmSpec{adaptStateful([](ConfigParamRegistry const& options,
                                                 ControlService& control,
                                                 DeviceSpec const& spec) {
    std::vector<std::string> filenames;
    auto filename = options.get<std::string>("esd-file");
    auto nEvents = options.get<int>("events");

    if (filename.empty()) {
      LOG(error) << "Option --esd-file did not provide a filename";
      control.readyToQuit(QuitRequest::All);
      return adaptStateless([](RawDeviceService& service) {
        service.device()->WaitFor(std::chrono::milliseconds(1000));
      });
    }

    // If option starts with a @, we consider the file as text which contains a list of
    // files.
    if (filename.size() && filename[0] == '@') {
      try {
        filename.erase(0, 1);
        std::ifstream filelist(filename);
        while (std::getline(filelist, filename)) {
          filenames.push_back(filename);
        }
      } catch (...) {
        LOG(error) << "Unable to process file list: " << filename;
      }
    } else {
      filenames.push_back(filename);
    }

    uint64_t readMask = calculateReadMask(spec.outputs, header::DataOrigin{"AOD"});
    auto counter = std::make_shared<unsigned int>(0);
    return adaptStateless([readMask,
                           counter,
                           filenames,
                           spec, nEvents](DataAllocator& outputs, ControlService& ctrl, RawDeviceService& service) {
      if (*counter >= filenames.size()) {
        LOG(info) << "All input files processed";
        ctrl.endOfStream();
        ctrl.readyToQuit(QuitRequest::Me);
        return;
      }
      auto f = filenames[*counter];
      setenv("O2RUN2CONVERTER", "run2ESD2Run3AOD", 0);
      auto command = std::string(getenv("O2RUN2CONVERTER"));
      if (nEvents > 0) {
        command += fmt::format(" -n {} ", nEvents);
      }
      FILE* pipe = popen((command + " " + f).c_str(), "r");
      if (pipe == nullptr) {
        LOG(ERROR) << "Unable to run converter: " << (command + " " + f).c_str() << f;
        ctrl.endOfStream();
        ctrl.readyToQuit(QuitRequest::All);
        return;
      }
      *counter += 1;

      /// We keep reading until the popen is not empty.
      while ((feof(pipe) == false) && (ferror(pipe) == 0)) {
        // Skip extra 0-padding...
        int c = fgetc(pipe);
        if (c == 0 || c == EOF) {
          continue;
        }
        ungetc(c, pipe);

        std::shared_ptr<arrow::RecordBatchReader> reader;
        auto input = std::make_shared<FileStream>(pipe);
        auto readerStatus = arrow::ipc::RecordBatchStreamReader::Open(input, &reader);
        if (readerStatus.ok() == false) {
          LOG(ERROR) << "Reader status not ok: " << readerStatus.message();
          break;
        }

        while (true) {
          std::shared_ptr<arrow::RecordBatch> batch;
          auto status = reader->ReadNext(&batch);
          if (batch.get() == nullptr) {
            break;
          }
          std::unordered_map<std::string, std::string> meta;
          batch->schema()->metadata()->ToUnorderedMap(&meta);
          std::shared_ptr<arrow::ipc::RecordBatchWriter> writer;
          if (meta["description"] == "TRACKPAR" && (readMask & AODTypeMask::Tracks)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "TRACKPAR"}, batch->schema());
          } else if (meta["description"] == "TRACKPARCOV" && (readMask & AODTypeMask::TracksCov)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "TRACKPARCOV"}, batch->schema());
          } else if (meta["description"] == "TRACKEXTRA" && (readMask & AODTypeMask::TracksExtra)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "TRACKEXTRA"}, batch->schema());
          } else if (meta["description"] == "CALO" && (readMask & AODTypeMask::Calo)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "CALO"}, batch->schema());
          } else if (meta["description"] == "MUON" && (readMask & AODTypeMask::Muon)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "MUON"}, batch->schema());
          } else if (meta["description"] == "VZERO" && (readMask & AODTypeMask::VZero)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "VZERO"}, batch->schema());
          } else if (meta["description"] == "ZDC" && (readMask & AODTypeMask::Zdc)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "ZDC"}, batch->schema());
          } else if (meta["description"] == "TRIGGER" && (readMask & AODTypeMask::Trigger)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "TRIGGER"}, batch->schema());
          } else if (meta["description"] == "COLLISION" && (readMask & AODTypeMask::Collisions)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "COLLISION"}, batch->schema());
          } else if (meta["description"] == "TIMEFRAME" && (readMask & AODTypeMask::Timeframe)) {
            writer = outputs.make<arrow::ipc::RecordBatchWriter>(Output{"AOD", "TIMEFRAME"}, batch->schema());
          } else {
            continue;
          }
          auto writeStatus = writer->WriteRecordBatch(*batch);
          if (writeStatus.ok() == false) {
            throw std::runtime_error("Error while writing record");
          }
        }
      }
      if (ferror(pipe)) {
        LOG(ERROR) << "Error while reading from PIPE";
      }
      pclose(pipe);
    });
  })};

  return callback;
}

AlgorithmSpec AODReaderHelpers::rootFileReaderCallback()
{
  auto callback = AlgorithmSpec{adaptStateful([](ConfigParamRegistry const& options,
                                                 DeviceSpec const& spec) {
    std::vector<std::string> filenames;
    auto filename = options.get<std::string>("aod-file");

    // If option starts with a @, we consider the file as text which contains a list of
    // files.
    if (filename.size() && filename[0] == '@') {
      try {
        filename.erase(0, 1);
        std::ifstream filelist(filename);
        while (std::getline(filelist, filename)) {
          filenames.push_back(filename);
        }
      } catch (...) {
        LOG(error) << "Unable to process file list: " << filename;
      }
    } else {
      filenames.push_back(filename);
    }

    // analyze type of requested tables
    uint64_t readMask = calculateReadMask(spec.outputs, header::DataOrigin{"AOD"});
    std::vector<OutputRoute> unknowns;
    if (readMask & AODTypeMask::Unknown)
      unknowns = getListOfUnknown(spec.outputs);

    auto counter = std::make_shared<int>(0);
    return adaptStateless([readMask,
                           unknowns,
                           counter,
                           filenames](DataAllocator& outputs, ControlService& control, DeviceSpec const& device) {
      // Each parallel reader reads the files whose index is associated to
      // their inputTimesliceId
      assert(device.inputTimesliceId < device.maxInputTimeslices);
      size_t fi = (*counter * device.maxInputTimeslices) + device.inputTimesliceId;
      if (fi >= filenames.size()) {
        LOG(info) << "All input files processed";
        control.endOfStream();
        control.readyToQuit(QuitRequest::Me);
        return;
      }
      auto f = filenames[fi];
      LOG(INFO) << "Processing " << f;
      auto infile = std::make_unique<TFile>(f.c_str());
      *counter += 1;
      if (infile.get() == nullptr || infile->IsOpen() == false) {
        LOG(ERROR) << "File not found: " + f;
        return;
      }
      auto tableMaker = [&infile, &readMask, &outputs, &f](auto metadata, AODTypeMask mask, char const* treeName) {
        if (readMask & mask) {
          using table_t = typename decltype(metadata)::table_t;
          std::unique_ptr<TTreeReader> reader = std::make_unique<TTreeReader>(treeName, infile.get());
          if (reader->IsInvalid()) {
            LOGP(ERROR, "Requested {} tree not found in file {}", treeName, f);
          } else {
            auto& builder = outputs.make<TableBuilder>(Output{decltype(metadata)::origin(), decltype(metadata)::description()});
            RootTableBuilderHelpers::convertASoA<table_t>(builder, *reader);
          }
        }
      };
      tableMaker(o2::aod::CollisionsMetadata{}, AODTypeMask::Collisions, "O2collisions");
      tableMaker(o2::aod::TracksMetadata{}, AODTypeMask::Tracks, "O2tracks");
      tableMaker(o2::aod::TracksCovMetadata{}, AODTypeMask::TracksCov, "O2tracks");
      tableMaker(o2::aod::TracksExtraMetadata{}, AODTypeMask::TracksExtra, "O2tracks");
      tableMaker(o2::aod::CalosMetadata{}, AODTypeMask::Calo, "O2calo");
      tableMaker(o2::aod::MuonsMetadata{}, AODTypeMask::Muon, "O2muon");
      tableMaker(o2::aod::VZerosMetadata{}, AODTypeMask::VZero, "O2vzero");
      tableMaker(o2::aod::ZdcsMetadata{}, AODTypeMask::Zdc, "O2zdc");
      tableMaker(o2::aod::TriggersMetadata{}, AODTypeMask::Trigger, "O2trigger");

      // tables not included in the DataModel
      if (readMask & AODTypeMask::Unknown) {

        // loop over unknowns
        for (auto route : unknowns) {
          auto concrete = DataSpecUtils::asConcreteDataMatcher(route.matcher);

          // get the tree from infile
          auto trname = concrete.description.str;
          auto tr = (TTree*)infile.get()->Get(trname);
          if (!tr) {
            LOG(ERROR) << "Tree " << trname << "is not contained in file " << f;
            return;
          }

          // create a TreeToTable object
          auto h = header::DataHeader(concrete.description, concrete.origin, concrete.subSpec);
          auto o = Output(h);
          auto& t2t = outputs.make<TreeToTable>(o, tr);

          // fill the table
          t2t.Fill();
        }
      }
    });
  })};

  return callback;
}

} // namespace o2::framework::readers
