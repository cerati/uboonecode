////////////////////////////////////////////////////////////////////////
//
// Class:       RawDigitFilterUBooNE
// Module Type: producer
// File:        RawDigitFilterUBooNE_module.cc
//
//              The intent of this module is to filter out "bad" channels
//              in an input RawDigit data stream. In the current implementation,
//              "bad" is defined as the truncated rms for a channel crossing
//              a user controlled threshold
//
// Configuration parameters:
//
// DigitModuleLabel      - the source of the RawDigit collection
// TruncMeanFraction     - the fraction of waveform bins to discard when
//                         computing the means and rms
// RMSRejectionCut       - vector of maximum allowed rms values to keep channel
//
// Created by Tracy Usher (usher@slac.stanford.edu) on August 17, 2015
//
////////////////////////////////////////////////////////////////////////

#include <cmath>
#include <algorithm>
#include <vector>

#include "art/Framework/Core/EDProducer.h"
#include "art/Framework/Services/Registry/ServiceHandle.h" 
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Framework/Services/Optional/TFileService.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "Geometry/Geometry.h"
#include "Utilities/DetectorProperties.h"
#include "Utilities/TimeService.h"
#include "Utilities/SimpleTimeService.h"
#include "CalibrationDBI/Interface/IDetPedestalService.h"
#include "CalibrationDBI/Interface/IDetPedestalProvider.h"

#include "RawData/RawDigit.h"
#include "RawData/raw.h"

#include "TH1.h"
#include "TH2.h"
#include "TProfile.h"

class Propagator;

class RawDigitFilterUBooNE : public art::EDProducer
{
public:

    // Copnstructors, destructor.
    explicit RawDigitFilterUBooNE(fhicl::ParameterSet const & pset);
    virtual ~RawDigitFilterUBooNE();

    // Overrides.
    virtual void reconfigure(fhicl::ParameterSet const & pset);
    virtual void produce(art::Event & e);
    virtual void beginJob();
    virtual void endJob();

private:

    // Fcl parameters.
    std::string          fDigitModuleLabel;      ///< The full collection of hits
    float                fTruncMeanFraction;     ///< Fraction for truncated mean
    std::vector<double>  fRmsRejectionCut;       ///< channel upper rms cut
    std::vector<double>  fRmsRejectionCutLow;    ///< channel lower rms cut
    unsigned int         fTheChosenWire;         ///< For example hist
    double               fMaxPedestalDiff;       ///< Max pedestal diff to db to warn
    bool                 fSmoothCorrelatedNoise; ///< Should we smooth the noise?
    size_t               fNumWiresToGroup;       ///< If smoothing, the number of wires to look at

    // Statistics.
    int fNumEvent;        ///< Number of events seen.
    
    // Pointers to the histograms we'll create for monitoring what is happening
    TH1D*     fAdcCntHist[3];
    TH1D*     fAveValHist[3];
    TH1D*     fRmsValHist[3];
    TH1D*     fPedValHist[3];
    TH1D*     fAverageHist[3];
    TProfile* fRmsValProf[3];
    TProfile* fPedValProf[3];
    
    
    bool      fFirstEvent;
    
    // Useful services, keep copies for now (we can update during begin run periods)
    art::ServiceHandle<geo::Geometry>            fGeometry;             ///< pointer to Geometry service
    art::ServiceHandle<util::DetectorProperties> fDetectorProperties;   ///< Detector properties service
    const lariov::IDetPedestalProvider&          fPedestalRetrievalAlg; ///< Keep track of an instance to the pedestal retrieval alg
};

DEFINE_ART_MODULE(RawDigitFilterUBooNE)

//----------------------------------------------------------------------------
/// Constructor.
///
/// Arguments:
///
/// pset - Fcl parameters.
///
RawDigitFilterUBooNE::RawDigitFilterUBooNE(fhicl::ParameterSet const & pset) :
                      fNumEvent(0),
                      fFirstEvent(true),
                      fPedestalRetrievalAlg(art::ServiceHandle<lariov::IDetPedestalService>()->GetPedestalProvider())

{
    reconfigure(pset);
    produces<std::vector<raw::RawDigit> >();

    // Report.
    mf::LogInfo("RawDigitFilterUBooNE") << "RawDigitFilterUBooNE configured\n";
}

//----------------------------------------------------------------------------
/// Destructor.
RawDigitFilterUBooNE::~RawDigitFilterUBooNE()
{}

//----------------------------------------------------------------------------
/// Reconfigure method.
///
/// Arguments:
///
/// pset - Fcl parameter set.
///
void RawDigitFilterUBooNE::reconfigure(fhicl::ParameterSet const & pset)
{
    fDigitModuleLabel      = pset.get<std::string>        ("DigitModuleLabel",                                       "daq");
    fTruncMeanFraction     = pset.get<float>              ("TruncMeanFraction",                                        0.2);
    fRmsRejectionCut       = pset.get<std::vector<double>>("RMSRejectonCut",      std::vector<double>() = {10.0,10.0, 5.0});
    fRmsRejectionCutLow    = pset.get<std::vector<double>>("RMSRejectonCutLow",   std::vector<double>() = {0.75,0.75,0.75});
    fTheChosenWire         = pset.get<unsigned int>       ("TheChosenWire",                                           1200);
    fMaxPedestalDiff       = pset.get<double>             ("MaxPedestalDiff",                                          10.);
    fSmoothCorrelatedNoise = pset.get<bool>               ("SmoothCorrelatedNoise",                                   true);
    fNumWiresToGroup       = pset.get<size_t>             ("NumWiresToGroup",                                           48);
}

//----------------------------------------------------------------------------
/// Begin job method.
void RawDigitFilterUBooNE::beginJob()
{
    // Access ART's TFileService, which will handle creating and writing
    // histograms and n-tuples for us.
    art::ServiceHandle<art::TFileService> tfs;
    
    // Define the histograms. Putting semi-colons around the title
    // causes it to be displayed as the x-axis label if the histogram
    // is drawn.
    fAdcCntHist[0]  = tfs->make<TH1D>("CntUPlane", ";#adc",  200, 9000., 10000.);
    fAdcCntHist[1]  = tfs->make<TH1D>("CntVPlane", ";#adc",  200, 9000., 10000.);
    fAdcCntHist[2]  = tfs->make<TH1D>("CntWPlane", ";#adc",  200, 9000., 10000.);
    fAveValHist[0]  = tfs->make<TH1D>("AveUPlane", ";Ave",   120,  -30.,    30.);
    fAveValHist[1]  = tfs->make<TH1D>("AveVPlane", ";Ave",   120,  -30.,    30.);
    fAveValHist[2]  = tfs->make<TH1D>("AveWPlane", ";Ave",   120,  -30.,    30.);
    fRmsValHist[0]  = tfs->make<TH1D>("RmsUPlane", ";RMS",   200,    0.,    50.);
    fRmsValHist[1]  = tfs->make<TH1D>("RmsVPlane", ";RMS",   200,    0.,    50.);
    fRmsValHist[2]  = tfs->make<TH1D>("RmsWPlane", ";RMS",   200,    0.,    50.);
    fPedValHist[0]  = tfs->make<TH1D>("PedUPlane", ";Ped",   200,  1950,  2150.);
    fPedValHist[1]  = tfs->make<TH1D>("PedVPlane", ";Ped",   200,  1950,  2150.);
    fPedValHist[2]  = tfs->make<TH1D>("PedWPlane", ";Ped",   200,   350,   550.);
    
    fRmsValProf[0]  = tfs->make<TProfile>("RmsUPlaneProf",  ";Wire #",  2400, 0., 2400., 0., 100.);
    fRmsValProf[1]  = tfs->make<TProfile>("RmsVPlaneProf",  ";Wire #",  2400, 0., 2400., 0., 100.);
    fRmsValProf[2]  = tfs->make<TProfile>("RmsWPlaneProf",  ";Wire #",  3456, 0., 3456., 0., 100.);

    fPedValProf[0]  = tfs->make<TProfile>("PedUPlaneProf",  ";Wire #",  2400, 0., 2400., 1500., 2500.);
    fPedValProf[1]  = tfs->make<TProfile>("PedVPlaneProf",  ";Wire #",  2400, 0., 2400., 1500., 2500.);
    fPedValProf[2]  = tfs->make<TProfile>("PedWPlaneProf",  ";Wire #",  3456, 0., 3456.,    0., 1000.);
    
    fAverageHist[0] = tfs->make<TH1D>("AverageU", ";Bin", 1000, 1500., 2500.);
    fAverageHist[1] = tfs->make<TH1D>("AverageV", ";Bin", 1000, 1500., 2500.);
    fAverageHist[2] = tfs->make<TH1D>("AverageW", ";Bin", 1000,    0., 1000.);
}

//----------------------------------------------------------------------------
/// Produce method.
///
/// Arguments:
///
/// evt - Art event.
///
/// This is the primary method.
///
void RawDigitFilterUBooNE::produce(art::Event & event)
{
    ++fNumEvent;
    
    // Agreed convention is to ALWAYS output to the event store so get a pointer to our collection
    std::unique_ptr<std::vector<raw::RawDigit> > filteredRawDigit(new std::vector<raw::RawDigit>);
    
    // Read in the digit List object(s).
    art::Handle< std::vector<raw::RawDigit> > digitVecHandle;
    event.getByLabel(fDigitModuleLabel, digitVecHandle);
    
    // Require a valid handle
    if (digitVecHandle.isValid())
    {
        unsigned int maxChannels    = fGeometry->Nchannels();
        unsigned int maxTimeSamples = fDetectorProperties->NumberTimeSamples();
    
        // Ok, to do the correlated noise removal we are going to need a rather impressive data structure...
        // Because we need to unpack each wire's data, we will need to "explode" it out into a data structure
        // here... with the good news that we'll release the memory at the end of the module so should not
        // impact downstream processing (I hope!).
        // What we are going to do is make a vector over views of vectors over wires of vectors over time samples
        std::vector<std::vector<raw::RawDigit::ADCvector_t>> rawDataViewWireTimeVec;
        std::vector<std::vector<float>>                      rawDataViewWireNoiseVec;
        std::vector<std::vector<float>>                      pedestalViewWireVec;
        std::vector<std::vector<raw::ChannelID_t>>           channelViewWireVec;
    
        // Initialize outer range to number of views
        rawDataViewWireTimeVec.resize(3);
        rawDataViewWireNoiseVec.resize(3);
        pedestalViewWireVec.resize(3);
        channelViewWireVec.resize(3);
    
        // Basic initialization goes here:
        for(size_t viewIdx = 0; viewIdx < 3; viewIdx++)
        {
            // For each view we need to presize the vector to the number of wires
            rawDataViewWireTimeVec[viewIdx].resize(fGeometry->Nwires(viewIdx));
            rawDataViewWireNoiseVec[viewIdx].resize(fGeometry->Nwires(viewIdx));
            pedestalViewWireVec[viewIdx].resize(fGeometry->Nwires(viewIdx));
            channelViewWireVec[viewIdx].resize(fGeometry->Nwires(viewIdx));
        }
    
        // Commence looping over raw digits
        for(size_t rdIter = 0; rdIter < digitVecHandle->size(); ++rdIter)
        {
            // get the reference to the current raw::RawDigit
            art::Ptr<raw::RawDigit> digitVec(digitVecHandle, rdIter);
        
            raw::ChannelID_t channel = digitVec->Channel();
        
            bool goodChan(true);
        
            // The below try-catch block may no longer be necessary
            // Decode the channel and make sure we have a valid one
            std::vector<geo::WireID> wids;
            try {
                wids = fGeometry->ChannelToWire(channel);
            }
            catch(...)
            {
                //std::cout << "===>> Found illegal channel with id: " << channel << std::endl;
                goodChan = false;
            }
        
            if (channel >= maxChannels || !goodChan) continue;
        
            // Recover plane and wire in the plane
            unsigned int view = wids[0].Plane;
            unsigned int wire = wids[0].Wire;
        
            unsigned int dataSize = digitVec->Samples();
            
            maxTimeSamples = std::min(maxTimeSamples, dataSize);
        
            // vector holding uncompressed adc values
            std::vector<short>& rawadc = rawDataViewWireTimeVec[view][wire];
        
            channelViewWireVec[view][wire] = channel;
        
            rawadc.resize(maxTimeSamples);
        
            // And now uncompress
            raw::Uncompress(digitVec->ADCs(), rawadc, digitVec->Compression());

            // The strategy for finding the average for a given wire will be to
            // find the most populated bin and the average using the neighboring bins
            // To do this we'll use a map with key the bin number and data the count in that bin
            // Define the map first
            std::map<short,short> binAdcMap;
        
            // Populate the mape
            for(const auto& adcVal : rawadc)
            {
                binAdcMap[adcVal]++;
            }
        
            // Find the max bin
            short binMax(-1);
            short binMaxCnt(0);
        
            for(const auto& binAdcItr : binAdcMap)
            {
                if (binAdcItr.second > binMaxCnt)
                {
                    binMax    = binAdcItr.first;
                    binMaxCnt = binAdcItr.second;
                }
            }
        
            // fill example hists - throw away code
            if (fFirstEvent && wire == fTheChosenWire)
            {
                for(const auto& binAdcItr : binAdcMap)
                {
                    fAverageHist[view]->Fill(binAdcItr.first, binAdcItr.second);
                }
            }
        
            // Armed with the max bin and its count, now set up to get an average
            // about this bin. We'll want to cut off at some user defined fraction
            // of the total bins on the wire
            int minNumBins = (1. - fTruncMeanFraction) * dataSize - 1;
            int curBinCnt(binMaxCnt);
        
            double peakValue(curBinCnt * binMax);
            double truncMean(peakValue);
        
            short binOffset(1);
        
            // This loop to develop the average
            // In theory, we could also keep the sum of the squares for the rms but I had problems doing
            // it that way so will loop twice... (potential time savings goes here!)
            while(curBinCnt < minNumBins)
            {
                if (binAdcMap[binMax-binOffset])
                {
                    curBinCnt += binAdcMap[binMax-binOffset];
                    truncMean += double(binAdcMap[binMax-binOffset] * (binMax - binOffset));
                }
            
                if (binAdcMap[binMax+binOffset])
                {
                    curBinCnt += binAdcMap[binMax+binOffset];
                    truncMean += double(binAdcMap[binMax+binOffset] * (binMax + binOffset));
                }
            
                binOffset++;
            }
        
            truncMean /= double(curBinCnt);
        
            binOffset  = 1;
        
            int    rmsBinCnt(binMaxCnt);
            double rmsVal(double(binMax)-truncMean);
        
            rmsVal *= double(rmsBinCnt) * rmsVal;
        
            // Second loop to get the rms
            while(rmsBinCnt < minNumBins)
            {
                if (binAdcMap[binMax-binOffset] > 0)
                {
                    int    binIdx  = binMax - binOffset;
                    int    binCnt  = binAdcMap[binIdx];
                    double binVals = double(binIdx) - truncMean;
                
                    rmsBinCnt += binCnt;
                    rmsVal    += double(binCnt) * binVals * binVals;
                }
            
                if (binAdcMap[binMax+binOffset] > 0)
                {
                    int    binIdx  = binMax + binOffset;
                    int    binCnt  = binAdcMap[binIdx];
                    double binVals = double(binIdx) - truncMean;
                
                    rmsBinCnt += binCnt;
                    rmsVal    += double(binCnt) * binVals * binVals;
                }
            
                binOffset++;
            }
        
            rmsVal = std::sqrt(std::max(0.,rmsVal / double(rmsBinCnt)));
        
            rawDataViewWireNoiseVec[view][wire] = rmsVal;

            // Recover the database version of the pedestal
            float pedestal = fPedestalRetrievalAlg.PedMean(channel);
        
            pedestalViewWireVec[view][wire] = pedestal;
        
            // Fill some histograms here
            fAdcCntHist[view]->Fill(curBinCnt, 1.);
            fAveValHist[view]->Fill(std::max(-29.9, std::min(29.9,truncMean - pedestal)), 1.);
            fRmsValHist[view]->Fill(std::min(49.9, rmsVal), 1.);
            fRmsValProf[view]->Fill(wire, rmsVal, 1.);
            fPedValProf[view]->Fill(wire, truncMean, 1.);
            fPedValHist[view]->Fill(truncMean, 1.);

            // Output a message is there is significant different to the pedestal
            if (abs(truncMean - pedestal) > fMaxPedestalDiff)
            {
                mf::LogInfo("RawDigitFilterUBooNE") << ">>> Pedestal mismatch, channel: " << channel << ", new value: " << truncMean << ", original: " << pedestal << ", rms: " << rmsVal << std::endl;
            }
            
            // If not smoothing
            if (!fSmoothCorrelatedNoise)
            {
                // Keep the RawDigit if below our rejection cut
                if (rmsVal < fRmsRejectionCut[view])
                {
                    filteredRawDigit->emplace_back(*digitVec);
                }
                else
                {
                    mf::LogInfo("RawDigitFilterUBooNE") <<  "--> Rejecting channel for large rms, channel: " << channel << ", rmsVal: " << rmsVal << ", truncMean: " << truncMean << ", pedestal: " << pedestal << std::endl;
                }
            }
        }
    
        // Try to implement Corey's algorithm here
        // The basic idea is to try to take groups of wires and find a metric within a given time bin
        // to use to correct the adc values on the wire
        // Make sure we want to do this...
        if (fSmoothCorrelatedNoise)
        {
            size_t nWiresPerMotherBoard(fNumWiresToGroup);
    
            // Perform the outer loop over views
            for(size_t viewIdx = 0; viewIdx < 3; viewIdx++)
            {
                // How many groups of wires this view?
                size_t nMotherBoards = fGeometry->Nwires(viewIdx) / nWiresPerMotherBoard;
        
                // Loop over wires in group (probably a motherboard's worth)
                for(size_t mbIdx = 0; mbIdx < nMotherBoards; mbIdx++)
                {
                    // Now we loop over the number of time bins (samples)
                    for(size_t sampleIdx = 0; sampleIdx < maxTimeSamples; sampleIdx++)
                    {
                        // Define a vector for accumulating values...
                        std::map<short,size_t> adcValuesMap;
                
                        // Finally, inside of here we are looping over wires on a motherboard
                        for(size_t wireIdx = 0; wireIdx < nWiresPerMotherBoard; wireIdx++)
                        {
                            // Recover the physical wire
                            size_t physWireIdx = nWiresPerMotherBoard * mbIdx + wireIdx;
                    
                            // If this wire is too noisy, or not enough noisy, reject
                            double rmsNoise(rawDataViewWireNoiseVec[viewIdx][physWireIdx]);

                            // Don't select "bad" wires, they are lost anyway
                            // Also, it is pointless to try to include the "ultra low noise" channels (or correct them)
                            if (rmsNoise > fRmsRejectionCutLow[viewIdx] && rmsNoise < fRmsRejectionCut[viewIdx])
                            {
                                float     adcLessPed = float(rawDataViewWireTimeVec[viewIdx][physWireIdx][sampleIdx]) - pedestalViewWireVec[viewIdx][physWireIdx];
                                short int adcValue   = std::round(10. * adcLessPed);
                        
                                // Make a poor man's cut on expected noise so we don't accidently include real pulses
                                if (fabs(adcLessPed) < 5.*rmsNoise) adcValuesMap[adcValue]++;
                            }
                        }
                
                        // Guard against an entire MB being bad
                        if (adcValuesMap.empty())
                        {
                            if (sampleIdx > 10) break;
                            continue;
                        }
                
                        // Find the most probable value
                        short int maxAdcValue(0);
                        size_t    maxAdcCnt(0);
                
                        for(const auto& adcValItr : adcValuesMap)
                        {
                            if (adcValItr.second > maxAdcCnt)
                            {
                                maxAdcValue = adcValItr.first;
                                maxAdcCnt   = adcValItr.second;
                            }
                        }
                
                        float mostProbableValue = 0.1 * float(maxAdcValue);
                
                        // Now run through and apply correction
                        for(size_t wireIdx = 0; wireIdx < nWiresPerMotherBoard; wireIdx++)
                        {
                            size_t physWireIdx = nWiresPerMotherBoard * mbIdx + wireIdx;
                    
                            // If this wire is too noisy, or not enough noisy, reject
                            double rmsNoise(rawDataViewWireNoiseVec[viewIdx][physWireIdx]);
                    
                            if (rmsNoise > fRmsRejectionCutLow[viewIdx] && rmsNoise < fRmsRejectionCut[viewIdx])
                            {
                                // Probably doesn't matter, but try to get slightly more accuracy by doing float math and rounding
                                float     newAdcValueFloat = float(rawDataViewWireTimeVec[viewIdx][physWireIdx][sampleIdx]) - mostProbableValue;
                                short int newAdcValue      = std::round(newAdcValueFloat);
                        
                                rawDataViewWireTimeVec[viewIdx][physWireIdx][sampleIdx] = newAdcValue;
                            }
                        }
                    }
            
                    // One more pass through to store the good channels
                    for(size_t wireIdx = 0; wireIdx < nWiresPerMotherBoard; wireIdx++)
                    {
                        // Recover the physical wire
                        size_t physWireIdx = nWiresPerMotherBoard * mbIdx + wireIdx;
                
                        // If this wire is too noisy, or not enough noisy, reject
                        double rmsNoise(rawDataViewWireNoiseVec[viewIdx][physWireIdx]);
                
                        if (rmsNoise < fRmsRejectionCut[viewIdx])
                        {
                            filteredRawDigit->emplace_back(raw::RawDigit(channelViewWireVec[viewIdx][physWireIdx], maxTimeSamples, rawDataViewWireTimeVec[viewIdx][physWireIdx], raw::kNone));
                        }
                    }
                }
            }
        }
    }
    
    // Reset this silly flag so we only fill our example hists once...
    fFirstEvent = false;
    
    // Add tracks and associations to event.
    event.put(std::move(filteredRawDigit));
}

//----------------------------------------------------------------------------
/// End job method.
void RawDigitFilterUBooNE::endJob()
{
    mf::LogInfo("RawDigitFilterUBooNE") << "Looked at " << fNumEvent << " events" << std::endl;
}