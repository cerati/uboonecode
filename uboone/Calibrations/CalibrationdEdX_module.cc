////////////////////////////////////////////////////////////////////////
// Class:       CalibrationdEdX
// Module Type: producer
// File:        CalibrationdEdX_module.cc
//
// Generated at Thu Nov 30 15:55:16 2017 by Tingjun Yang using artmod
// from cetpkgsupport v1_13_00.
////////////////////////////////////////////////////////////////////////

#include "art/Framework/Core/EDProducer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Principal/SubRun.h"
#include "canvas/Utilities/InputTag.h"
#include "canvas/Persistency/Common/FindManyP.h"
#include "fhiclcpp/ParameterSet.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "lardata/Utilities/AssociationUtil.h"
#include "lardata/DetectorInfoServices/DetectorPropertiesService.h"
#include "larreco/Calorimetry/CalorimetryAlg.h"
#include "lardataobj/RecoBase/Track.h"
#include "lardataobj/AnalysisBase/Calorimetry.h"
#include "larcoreobj/SimpleTypesAndConstants/PhysicalConstants.h"
#include "uboone/Database/TPCEnergyCalib/TPCEnergyCalibService.h"
#include "uboone/Database/TPCEnergyCalib/TPCEnergyCalibProvider.h"


#include "TH2F.h"
#include "TH1F.h"
#include "TFile.h"

#include <memory>

namespace ub {
  class CalibrationdEdX;
}

class ub::CalibrationdEdX : public art::EDProducer {
public:
  explicit CalibrationdEdX(fhicl::ParameterSet const & p);
  // The destructor generated by the compiler is fine for classes
  // without bare pointers or other resource use.

  // Plugins should not be copied or assigned.
  CalibrationdEdX(CalibrationdEdX const &) = delete;
  CalibrationdEdX(CalibrationdEdX &&) = delete;
  CalibrationdEdX & operator = (CalibrationdEdX const &) = delete;
  CalibrationdEdX & operator = (CalibrationdEdX &&) = delete;

  // Required functions.
  void produce(art::Event & e) override;

  // Selected optional functions.
  void beginJob() override;

private:

  std::string fTrackModuleLabel;
  std::string fCalorimetryModuleLabel;
  std::string fCalibrationFileName;
  std::vector<std::string> fCorr_YZ;
  std::vector<std::string> fCorr_X;
  bool fUseRecoTrackDir;

  calo::CalorimetryAlg caloAlg;

  // modified box model parameters for data
  double fModBoxA;
  double fModBoxB;

  //histograms for calibration
  std::vector<TH2F*> hCorr_YZ;
  std::vector<TH1F*> hCorr_X;

  double GetYZCorrection(TVector3& xyz, TH2F *his);
  double GetXCorrection(TVector3& xyz, TH1F *his);

  const detinfo::DetectorProperties* detprop;

};


ub::CalibrationdEdX::CalibrationdEdX(fhicl::ParameterSet const & p)
  : fTrackModuleLabel      (p.get< std::string >("TrackModuleLabel"))
  , fCalorimetryModuleLabel(p.get< std::string >("CalorimetryModuleLabel"))
  , fCalibrationFileName   (p.get< std::string >("CalibrationFileName"))
  , fCorr_YZ               (p.get< std::vector<std::string> >("Corr_YZ"))
  , fCorr_X                (p.get< std::vector<std::string> >("Corr_X"))
  , fUseRecoTrackDir       (p.get< bool>("UseRecoTrackDir"))
  , caloAlg(p.get< fhicl::ParameterSet >("CaloAlg"))
  , fModBoxA               (p.get< double >("ModBoxA"))
  , fModBoxB               (p.get< double >("ModBoxB")) 
{
  // Call appropriate produces<>() functions here.
  if (fCorr_YZ.size()!=3 || fCorr_X.size()!=3){
    throw art::Exception(art::errors::Configuration)
      <<"Size of Corr_YZ and Corr_X need to be 3.";
  }
  detprop = art::ServiceHandle<detinfo::DetectorPropertiesService>()->provider();

  //create calorimetry product and its association with track
  produces< std::vector<anab::Calorimetry>              >();
  produces< art::Assns<recob::Track, anab::Calorimetry> >();

}

void ub::CalibrationdEdX::produce(art::Event & evt)
{

  //handle to tpc energy calibration provider
  const lariov::TPCEnergyCalibProvider& energyCalibProvider
    = art::ServiceHandle<lariov::TPCEnergyCalibService>()->GetProvider();

  //create anab::Calorimetry objects and make association with recob::Track
  std::unique_ptr< std::vector<anab::Calorimetry> > calorimetrycol(new std::vector<anab::Calorimetry>);
  std::unique_ptr< art::Assns<recob::Track, anab::Calorimetry> > assn(new art::Assns<recob::Track, anab::Calorimetry>);

  //get existing track/calorimetry objects
  art::Handle< std::vector<recob::Track> > trackListHandle;
  evt.getByLabel(fTrackModuleLabel,trackListHandle);

  std::vector<art::Ptr<recob::Track> > tracklist;
  art::fill_ptr_vector(tracklist, trackListHandle);

  art::FindManyP<anab::Calorimetry> fmcal(trackListHandle, evt, fCalorimetryModuleLabel);

  if (!fmcal.isValid()){
    throw art::Exception(art::errors::ProductNotFound)
      <<"Could not get assocated Calorimetry objects";
  }

  for (size_t trkIter = 0; trkIter < tracklist.size(); ++trkIter){   
    for (size_t i = 0; i<fmcal.at(trkIter).size(); ++i){
      auto & calo = fmcal.at(trkIter)[i];
      
      if (!(calo->dEdx()).size()){
        //empty calorimetry product, just copy it
        calorimetrycol->push_back(*calo);
        util::CreateAssn(*this, evt, *calorimetrycol, tracklist[trkIter], *assn);
      }
      else{
        //start calibrating dQdx

        //get original calorimetry information
        double                Kin_En     = calo->KineticEnergy();
        std::vector<double>   vdEdx      = calo->dEdx();
        std::vector<double>   vdQdx      = calo->dQdx();
        std::vector<double>   vresRange  = calo->ResidualRange();
        std::vector<double>   deadwire   = calo->DeadWireResRC();
        double                Trk_Length = calo->Range();
        std::vector<double>   fpitch     = calo->TrkPitchVec();
        std::vector<TVector3> vXYZ       = calo->XYZ();
        geo::PlaneID          planeID    = calo->PlaneID();

        //make sure the vectors are of the same size
        if (vdEdx.size()!=vXYZ.size()||
            vdQdx.size()!=vXYZ.size()||
            vresRange.size()!=vXYZ.size()||
            fpitch.size()!=vXYZ.size()){
          throw art::Exception(art::errors::Configuration)
      <<"Vector sizes mismatch for vdEdx, vdQdx, vresRange, fpitch, vXYZ";
        }

        //make sure the planeID is reasonable
        if (!planeID.isValid){
          throw art::Exception(art::errors::Configuration)
      <<"planeID is invalid";
        }
        if (planeID.Plane<0 || planeID.Plane>2){
          throw art::Exception(art::errors::Configuration)
            <<"plane is invalid "<<planeID.Plane;
        }
        bool flipdir = false;
        if (fUseRecoTrackDir){
          // we want to undo the direction flip done in Calorimetry_module.cc
          // use the reconstructed track end to calculate residual range
          TVector3 trkstartcalo = vXYZ[0];
          TVector3 trkendcalo = vXYZ.back();
          if (vresRange[0]<vresRange.back()){
            trkstartcalo = vXYZ.back();
            trkendcalo = vXYZ[0];
          }
          if ((trkstartcalo - tracklist[trkIter]->Vertex()).Mag()+(trkendcalo - tracklist[trkIter]->End()).Mag()>
              (trkstartcalo - tracklist[trkIter]->End()).Mag()+(trkendcalo - tracklist[trkIter]->Vertex()).Mag()){
            flipdir = true;
          }
        }

        for (size_t j = 0; j<vdQdx.size(); ++j){
	  float yzcorrection = energyCalibProvider.YZdqdxCorrection(planeID.Plane, vXYZ[j].Y(), vXYZ[j].Z());
	  float xcorrection  = energyCalibProvider.XdqdxCorrection(planeID.Plane, vXYZ[j].X());
	  if (!yzcorrection) yzcorrection = 1.0;
	  if (!xcorrection) xcorrection = 1.0;
	  
          /*double alt_yzcorrection = GetYZCorrection(vXYZ[j], hCorr_YZ[planeID.Plane]);
          double alt_xcorrection = GetXCorrection(vXYZ[j], hCorr_X[planeID.Plane]);
	  
	  if (fabs((alt_xcorrection-xcorrection)/alt_xcorrection)>1.0e-5) {
	    std::cout << "X correction constants do not match: \n"
	                                            << "  "<<xcorrection<<" vs "<<alt_xcorrection
						    << "\n  at plane "<<planeID.Plane<<" and coords "<<vXYZ[j].X()<<","<<vXYZ[j].Y()<<","<<vXYZ[j].Z()<<"\n";
	  }
	  if (fabs((alt_yzcorrection-yzcorrection)/alt_yzcorrection)>1.0e-5) {
	    std::cout << "YZ correction constants do not match: \n"
	                                            << "  "<<yzcorrection<<" vs "<<alt_yzcorrection
						    << "\n  at plane "<<planeID.Plane<<" and coords "<<vXYZ[j].X()<<","<<vXYZ[j].Y()<<","<<vXYZ[j].Z()<<"\n";
	  }*/
	  
          vdQdx[j] = yzcorrection*xcorrection*vdQdx[j];
          /*
          //set time to be trgger time so we don't do lifetime correction
          //we will turn off lifetime correction in caloAlg, this is just to be double sure
          vdEdx[j] = caloAlg.dEdx_AREA(vdQdx[j], detprop->TriggerOffset(), planeID.Plane, 0);
          */

          //Calculate dE/dx using the new recombination constants
          double dQdx_e = caloAlg.ElectronsFromADCArea(vdQdx[j], planeID.Plane);
          double rho = detprop->Density();            // LAr density in g/cm^3
          double Wion = 1000./util::kGeVToElectrons;  // 23.6 eV = 1e, Wion in MeV/e
          double E_field = detprop->Efield();        // Electric Field in the drift region in KV/cm
          double Beta = fModBoxB / (rho * E_field);
          double Alpha = fModBoxA;
          vdEdx[j] = (exp(Beta * Wion * dQdx_e ) - Alpha) / Beta;

          if (flipdir) {
            double rr = Trk_Length - vresRange[j];
            if (rr<0) rr = 0;
            if (rr>Trk_Length) rr = Trk_Length;
            vresRange[j] = rr;
          }
        }
        //save new calorimetry information 
        calorimetrycol->push_back(anab::Calorimetry(Kin_En,
                                                    vdEdx,
                                                    vdQdx,
                                                    vresRange,
                                                    deadwire,
                                                    Trk_Length,
                                                    fpitch,
                                                    vXYZ,
                                                    planeID));
        util::CreateAssn(*this, evt, *calorimetrycol, tracklist[trkIter], *assn);
      }//calorimetry object not empty
    }//loop over calorimetry objects
  }//loop over tracks

  evt.put(std::move(calorimetrycol));
  evt.put(std::move(assn));

  return;
}

void ub::CalibrationdEdX::beginJob()
{
  // Implementation of optional member function here.

  // Open root file with YZ and X corrections
  cet::search_path sp("FW_SEARCH_PATH");
  std::string fROOTfile;
  if( !sp.find_file(fCalibrationFileName, fROOTfile) )
    throw cet::exception("CalibrationdEdX") << "cannot find the calibration root file: \n" 
					   << fROOTfile
					   << "\n bail ungracefully.\n";

  TFile f(fROOTfile.c_str());

  // Read in correction histograms
  for (size_t i = 0; i<fCorr_YZ.size(); ++i){
    hCorr_YZ.push_back((TH2F*)f.Get(fCorr_YZ[i].c_str()));
    if (!hCorr_YZ.back()){
      throw art::Exception(art::errors::Configuration)
        <<"Could not find histogram "<<fCorr_YZ[i]<<" in "<<fCalibrationFileName;
    }
    hCorr_X.push_back((TH1F*)f.Get(fCorr_X[i].c_str()));
    if (!hCorr_X.back()){
      throw art::Exception(art::errors::Configuration)
        <<"Could not find histogram "<<fCorr_X[i]<<" in "<<fCalibrationFileName;
    }
  }
}
                            
double ub::CalibrationdEdX::GetYZCorrection(TVector3& xyz, TH2F *his){

  if (!his){
    throw art::Exception(art::errors::Configuration)
      <<"Histogram is empty";
  }
  int biny = his->GetYaxis()->FindBin(xyz[1]);
  if (biny == 0) biny = 1;
  if (biny == his->GetNbinsY()+1) biny = his->GetNbinsY();

  int binz = his->GetXaxis()->FindBin(xyz[2]);
  if (binz == 0) binz = 1;
  if (binz == his->GetNbinsX()+1) binz = his->GetNbinsX();

  double corr = his->GetBinContent(binz, biny);

  if (corr) return corr;
  else return 1.0;

  //looking at neighboring bins
  /*for (int i = biny + 1; i <= his->GetNbinsY(); ++i){
    if (his->GetBinContent(binz, i)){
      return his->GetBinContent(binz, i);
    }
  }
  
  for (int i = biny - 1; i >= 1; --i){
    if (his->GetBinContent(binz, i)){
      return his->GetBinContent(binz, i);
    }
  }
  
  for (int i = binz + 1; i <= his->GetNbinsX(); ++i){
    if (his->GetBinContent(i, biny)){
      return his->GetBinContent(i, biny);
    }
  }
  
  for (int i = binz - 1; i >= 1; --i){
    if (his->GetBinContent(i, biny)){
      return his->GetBinContent(i, biny);
    }
  }
  
  //no nonzero correction found? just return 1
  return 1.;*/
  
}

double ub::CalibrationdEdX::GetXCorrection(TVector3& xyz, TH1F *his){

  if (!his){
    throw art::Exception(art::errors::Configuration)
      <<"Histogram is empty";
  }

  int bin = his->GetXaxis()->FindBin(xyz[0]);
  if (bin == 0) bin = 1;
  if (bin == his->GetNbinsX()+1) bin = his->GetNbinsX();

  return his->GetBinContent(bin);

}

DEFINE_ART_MODULE(ub::CalibrationdEdX)
