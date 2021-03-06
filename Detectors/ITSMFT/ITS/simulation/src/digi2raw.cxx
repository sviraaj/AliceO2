// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file digi2raw.cxx
/// \author ruben.shahoyan@cern.ch

#include <boost/program_options.hpp>
#include <TTree.h>
#include <TChain.h>
#include <TFile.h>
#include <TStopwatch.h>
#include "Framework/Logger.h"
#include <vector>
#include <string>
#include <iomanip>
#include "ITSMFTReconstruction/ChipMappingITS.h"
#include "ITSMFTReconstruction/GBTWord.h"
#include "DataFormatsITSMFT/ROFRecord.h"
#include "DataFormatsParameters/GRPObject.h"
#include "DataFormatsITSMFT/Digit.h"
#include "ITSMFTSimulation/MC2RawEncoder.h"
#include "DetectorsCommonDataFormats/DetID.h"
#include "DetectorsCommonDataFormats/NameConf.h"
#include "CommonUtils/StringUtils.h"

/// MC->raw conversion with new (variable page size) format for ITS
using MAP = o2::itsmft::ChipMappingITS;
namespace bpo = boost::program_options;

void setupLinks(o2::itsmft::MC2RawEncoder<MAP>& m2r, std::string_view outDir, std::string_view outPrefix, bool filePerCRU);
void digi2raw(std::string_view inpName, std::string_view outDir, bool filePerCRU, int verbosity, int superPageSizeInB = 1024 * 1024);

int main(int argc, char** argv)
{
  bpo::variables_map vm;
  bpo::options_description opt_general("Usage:\n  " + std::string(argv[0]) +
                                       "Convert ITS digits to CRU raw data\n");
  bpo::options_description opt_hidden("");
  bpo::options_description opt_all;
  bpo::positional_options_description opt_pos;

  try {
    auto add_option = opt_general.add_options();
    add_option("help,h", "Print this help message");
    add_option("verbosity,v", bpo::value<uint32_t>()->default_value(0), "verbosity level [0 = no output]");
    add_option("input-file,i", bpo::value<std::string>()->default_value("itsdigits.root"), "input ITS digits file");
    add_option("file-per-cru,c", bpo::value<bool>()->default_value(false)->implicit_value(true), "create output file per CRU (default: per layer)");
    add_option("output-dir,o", bpo::value<std::string>()->default_value("./"), "Output directory for raw data");

    opt_all.add(opt_general).add(opt_hidden);
    bpo::store(bpo::command_line_parser(argc, argv).options(opt_all).positional(opt_pos).run(), vm);

    if (vm.count("help")) {
      std::cout << opt_general << std::endl;
      exit(0);
    }

    bpo::notify(vm);
  } catch (bpo::error& e) {
    std::cerr << "ERROR: " << e.what() << std::endl
              << std::endl;
    std::cerr << opt_general << std::endl;
    exit(1);
  } catch (std::exception& e) {
    std::cerr << e.what() << ", application will now exit" << std::endl;
    exit(2);
  }

  digi2raw(vm["input-file"].as<std::string>(),
           vm["output-dir"].as<std::string>(),
           vm["file-per-cru"].as<bool>(),
           vm["verbosity"].as<uint32_t>());

  return 0;
}

void digi2raw(std::string_view inpName, std::string_view outDir, bool filePerCRU, int verbosity, int superPageSizeInB)
{
  TStopwatch swTot;
  swTot.Start();
  using ROFR = o2::itsmft::ROFRecord;
  using ROFRVEC = std::vector<o2::itsmft::ROFRecord>;
  const uint8_t ruSWMin = 0, ruSWMax = 0xff; // seq.ID of 1st and last RU (stave) to convert
  ///-------> input
  std::string digTreeName{o2::base::NameConf::MCTTREENAME.data()};
  TChain digTree(digTreeName.c_str());
  digTree.AddFile(inpName.data());

  std::vector<o2::itsmft::Digit> digiVec, *digiVecP = &digiVec;
  std::string digBranchName = o2::utils::concat_string(MAP::getName(), "Digit");
  if (!digTree.GetBranch(digBranchName.c_str())) {
    LOG(FATAL) << "Failed to find the branch " << digBranchName << " in the tree " << digTreeName;
  }
  digTree.SetBranchAddress(digBranchName.c_str(), &digiVecP);

  // ROF record entries in the digit tree
  ROFRVEC rofRecVec, *rofRecVecP = &rofRecVec;
  std::string rofRecName = o2::utils::concat_string(MAP::getName(), "DigitROF");
  if (!digTree.GetBranch(rofRecName.c_str())) {
    LOG(FATAL) << "Failed to find the branch " << rofRecName << " in the tree " << digTreeName;
  }
  digTree.SetBranchAddress(rofRecName.c_str(), &rofRecVecP);
  ///-------< input
  std::string inputGRP = o2::base::NameConf::getGRPFileName();
  const auto grp = o2::parameters::GRPObject::loadFrom(inputGRP);

  o2::itsmft::MC2RawEncoder<MAP> m2r;
  m2r.setVerbosity(verbosity);
  m2r.setContinuousReadout(grp->isDetContinuousReadOut(MAP::getDetID())); // must be set explicitly
  m2r.setDefaultSinkName(o2::utils::concat_string(MAP::getName(), ".raw"));
  m2r.setMinMaxRUSW(ruSWMin, ruSWMax);
  m2r.getWriter().setSuperPageSize(superPageSizeInB);

  m2r.setVerbosity(verbosity);
  setupLinks(m2r, outDir, MAP::getName(), filePerCRU);
  //-------------------------------------------------------------------------------<<<<
  int lastTreeID = -1;
  long offs = 0, nEntProc = 0;
  for (int i = 0; i < digTree.GetEntries(); i++) {
    digTree.GetEntry(i);
    for (const auto& rofRec : rofRecVec) {
      int nDigROF = rofRec.getNEntries();
      if (verbosity) {
        LOG(INFO) << "Processing ROF:" << rofRec.getROFrame() << " with " << nDigROF << " entries";
        rofRec.print();
      }
      if (!nDigROF) {
        if (verbosity) {
          LOG(INFO) << "Frame is empty"; // ??
        }
        continue;
      }
      nEntProc++;
      auto dgs = nDigROF ? gsl::span<const o2::itsmft::Digit>(&digiVec[rofRec.getFirstEntry()], nDigROF) : gsl::span<const o2::itsmft::Digit>();
      m2r.digits2raw(dgs, rofRec.getBCData());
    }
  } // loop over multiple ROFvectors (in case of chaining)

  m2r.getWriter().writeConfFile(MAP::getName(), "RAWDATA", o2::utils::concat_string(outDir, '/', MAP::getName(), "raw.cfg"));
  m2r.finalize(); // finish TF and flush data
  //
  swTot.Stop();
  swTot.Print();
}

void setupLinks(o2::itsmft::MC2RawEncoder<MAP>& m2r, std::string_view outDir, std::string_view outPrefix, bool filePerCRU)
{
  //------------------------------------------------------------------------------->>>>
  // just as an example, we require here that the staves are read via 3 links, with partitioning according to lnkXB below
  // while OB staves use only 1 link.
  // Note, that if the RU container is not defined, it will be created automatically
  // during encoding.
  // If the links of the container are not defined, a single link readout will be assigned

  constexpr int MaxLinksPerRU = 3;
  constexpr int MaxLinksPerCRU = 16;
  const auto& mp = m2r.getMapping();
  int lnkAssign[3][MaxLinksPerRU] = {
    // requested link cabling for IB, MB and OB
    /* // uncomment this to have 1 link per RU
    {9, 0, 0}, // IB
    {16, 0, 0}, // MB
    {28, 0, 0} // OB
     */
    {3, 3, 3}, // IB
    {5, 5, 6}, // MB
    {9, 9, 10} // OB
  };

  // this is an arbitrary mapping
  int nCRU = 0, nRUtot = 0, nRU = 0, nLinks = 0;
  int linkID = 0, cruIDprev = -1, cruID = o2::detectors::DetID::ITS << 10; // this will be the lowest CRUID
  std::string outFileLink;

  for (int ilr = 0; ilr < mp.NLayers; ilr++) {
    int nruLr = mp.getNStavesOnLr(ilr);
    int ruType = mp.getRUType(nRUtot); // IB, MB or OB
    int* lnkAs = lnkAssign[ruType];
    // count requested number of links per RU
    int nlk = 0;
    for (int i = 3; i--;) {
      nlk += lnkAs[i] ? 1 : 0;
    }

    for (int ir = 0; ir < nruLr; ir++) {
      int ruID = nRUtot++;
      bool accept = !(ruID < m2r.getRUSWMin() || ruID > m2r.getRUSWMax()); // ignored RUs ?
      if (accept) {
        m2r.getCreateRUDecode(ruID); // create RU container
        nRU++;
      }
      int accL = 0;
      for (int il = 0; il < MaxLinksPerRU; il++) { // create links
        if (accept) {
          nLinks++;
          auto& ru = *m2r.getRUDecode(ruID);
          uint32_t lanes = mp.getCablesOnRUType(ru.ruInfo->ruType); // lanes patter of this RU
          ru.links[il] = m2r.addGBTLink();
          auto link = m2r.getGBTLink(ru.links[il]);
          link->lanes = lanes & ((0x1 << lnkAs[il]) - 1) << (accL);
          link->idInCRU = linkID;
          link->cruID = cruID;
          link->feeID = mp.RUSW2FEEId(ruID, il);
          link->endPointID = 0; // 0 or 1
          accL += lnkAs[il];
          if (m2r.getVerbosity()) {
            LOG(INFO) << "RU" << ruID << '(' << ir << " on lr " << ilr << ") " << link->describe()
                      << " -> " << outFileLink;
          }
          // register the link in the writer, if not done here, its data will be dumped to common default file
          outFileLink = filePerCRU ? o2::utils::concat_string(outDir, "/", outPrefix, "_cru", std::to_string(nCRU), ".raw") : o2::utils::concat_string(outDir, "/", outPrefix, "_lr", std::to_string(ilr), ".raw");
          m2r.getWriter().registerLink(link->feeID, link->cruID, link->idInCRU,
                                       link->endPointID, outFileLink);
          //
          if (cruIDprev != cruID) { // just to count used CRUs
            cruIDprev = cruID;
            nCRU++;
          }
        }
        if ((++linkID) >= MaxLinksPerCRU) {
          linkID = 0;
          ++cruID;
        }
      }
    }
    if (linkID) {
      linkID = 0; // we don't want to put links of different layers on the same CRU
      ++cruID;
    }
  }
  LOG(INFO) << "Distributed " << nLinks << " links on " << nRU << " RUs in " << nCRU << " CRUs";
}
