// A macro with the purpose to produce
// simulation files in the new layout, where detector hits
// are stored in individual files.
//
// Applicable to the monolithic simulation output produced by o2-sim-serial -- until
// this is moved to the new scheme, too.

// this is a generic code to copy branches from one to another tree
void copyBranch(const char* originfile, const char* targetfile, std::vector<std::string> const& branchnames)
{
  const char* TREENAME = "o2sim";
  //Get old file, old tree and set top branch address
  auto oldfile = TFile::Open(originfile);
  TTree* oldtree = nullptr;
  oldfile->GetObject(TREENAME, oldtree);

  if (oldtree) {
    // Deactivate all branches
    oldtree->SetBranchStatus("*", 0);

    // Activate the branches to be copied (our skim)
    for (auto& br : branchnames) {
      std::string regex = br + std::string("*");
      oldtree->SetBranchStatus(regex.c_str(), 1);
    }

    //Create a new file + a clone of old tree header.
    auto newfile = TFile::Open(targetfile, "RECREATE");
    auto newtree = oldtree->CloneTree(0);

    // Here we copy the branches
    newtree->CopyEntries(oldtree, oldtree->GetEntries());
    newtree->SetEntries(oldtree->GetEntries());
    // Flush to disk
    newtree->Write();
    newfile->Close();
    // delete newtree;
    delete newfile;
  }

  if (oldfile) {
    oldfile->Close();
    delete oldfile;
  }
}

void migrateSimFiles(const char* filebase = "o2sim")
{

  // READ GRP AND ITERATE OVER DETECTED PARTS
  auto grp = o2::parameters::GRPObject::loadFrom(o2::base::NameConf::getGRPFileName(filebase).c_str());
  if (!grp) {
    std::cerr << "No GRP found. Exiting\n";
  }

  // split off the kinematics file
  std::string originalfilename = std::string(filebase) + std::string(".root");
  auto kinematicsfile = o2::base::NameConf::getMCKinematicsFileName(filebase);
  copyBranch(originalfilename.c_str(), kinematicsfile.c_str(), o2::detectors::SimTraits::KINEMATICSBRANCHES);

  // loop over all possible detectors
  for (auto detid = o2::detectors::DetID::First; detid <= o2::detectors::DetID::Last; ++detid) {
    if (!grp->isDetReadOut(detid)) {
      continue;
    }
    // fetch possible sim branches for this detector and copy them
    auto simbranches = o2::detectors::SimTraits::DETECTORBRANCHNAMES[detid];
    copyBranch(originalfilename.c_str(), o2::base::NameConf::getHitsFileName(detid, filebase).c_str(), simbranches);
  }
}
