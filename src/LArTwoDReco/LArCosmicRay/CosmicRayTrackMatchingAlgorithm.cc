/**
 *  @file   LArContent/src/LArTwoDReco/LArCosmicRay/CosmicRayTrackMatchingAlgorithm.cc
 *
 *  @brief  Implementation of the cosmic ray splitting algorithm class.
 *
 *  $Log: $
 */

#include "Pandora/AlgorithmHeaders.h"

#include "LArTwoDReco/LArCosmicRay/CosmicRayTrackMatchingAlgorithm.h"

#include "LArHelpers/LArGeometryHelper.h"
#include "LArHelpers/LArClusterHelper.h"
#include "LArHelpers/LArThreeDHelper.h"

using namespace pandora;

namespace lar
{

StatusCode CosmicRayTrackMatchingAlgorithm::Run()
{
    // Get the available clusters for each view
    ClusterVector availableClustersU, availableClustersV, availableClustersW;
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->GetAvailableClusters(m_inputClusterListNameU, availableClustersU));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->GetAvailableClusters(m_inputClusterListNameV, availableClustersV));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->GetAvailableClusters(m_inputClusterListNameW, availableClustersW));

    // Select clean clusters in each view
    ClusterVector cleanClustersU, cleanClustersV, cleanClustersW;
    this->SelectCleanClusters(availableClustersU, cleanClustersU);
    this->SelectCleanClusters(availableClustersV, cleanClustersV);
    this->SelectCleanClusters(availableClustersW, cleanClustersW);

    // Build a map of sliding linear fit results
    TwoDSlidingFitResultMap slidingFitResultMap;
    this->AddToSlidingFitResultMap(cleanClustersU, slidingFitResultMap);
    this->AddToSlidingFitResultMap(cleanClustersV, slidingFitResultMap);
    this->AddToSlidingFitResultMap(cleanClustersW, slidingFitResultMap);

    // Perform matches between views and identify new clusters
    HitAssociationMap hitAssociationsU, hitAssociationsV, hitAssociationsW;
    ClusterAssociationMap clusterAssociationsU, clusterAssociationsV, clusterAssociationsW;
    this->SelectMatchedTracks(slidingFitResultMap, cleanClustersU, cleanClustersV, availableClustersW, hitAssociationsW, clusterAssociationsW);
    this->SelectMatchedTracks(slidingFitResultMap, cleanClustersV, cleanClustersW, availableClustersU, hitAssociationsU, clusterAssociationsU);
    this->SelectMatchedTracks(slidingFitResultMap, cleanClustersW, cleanClustersU, availableClustersV, hitAssociationsV, clusterAssociationsV);

    // Modify existing clusters and create new clusters
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ModifyClusters(m_inputClusterListNameU, hitAssociationsU, clusterAssociationsU));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ModifyClusters(m_inputClusterListNameV, hitAssociationsV, clusterAssociationsV));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ModifyClusters(m_inputClusterListNameW, hitAssociationsW, clusterAssociationsW));

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode CosmicRayTrackMatchingAlgorithm::GetAvailableClusters(const std::string inputClusterListNames, ClusterVector &clusterVector) const
{
    const ClusterList *pClusterList = NULL;
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::GetList(*this, inputClusterListNames, pClusterList))

    for (ClusterList::const_iterator cIter = pClusterList->begin(), cIterEnd = pClusterList->end(); cIter != cIterEnd; ++cIter)
    {
        Cluster *pCluster = *cIter;
        if (!pCluster->IsAvailable())
            continue;

        clusterVector.push_back(pCluster);
    }

    if (clusterVector.empty())
        return STATUS_CODE_NOT_FOUND;

    std::sort(clusterVector.begin(), clusterVector.end(), LArClusterHelper::SortByNHits);

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

void CosmicRayTrackMatchingAlgorithm::SelectCleanClusters(const ClusterVector &inputVector, ClusterVector &outputVector) const
{
    for (ClusterVector::const_iterator iter = inputVector.begin(), iterEnd = inputVector.end(); iter != iterEnd; ++iter)
    {
        Cluster *pCluster = *iter;

        if (LArClusterHelper::GetLengthSquared(pCluster) < m_clusterMinLength * m_clusterMinLength)
            continue;

        outputVector.push_back(pCluster);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------

void CosmicRayTrackMatchingAlgorithm::AddToSlidingFitResultMap(const ClusterVector &clusterVector,
    TwoDSlidingFitResultMap &slidingFitResultMap) const
{
    for (ClusterVector::const_iterator iter = clusterVector.begin(), iterEnd = clusterVector.end(); iter != iterEnd; ++iter)
    {
        if (slidingFitResultMap.end() == slidingFitResultMap.find(*iter))
        {
            TwoDSlidingFitResult slidingFitResult;
            LArClusterHelper::LArTwoDSlidingFit(*iter, m_halfWindowLayers, slidingFitResult);

            if (!slidingFitResultMap.insert(TwoDSlidingFitResultMap::value_type(*iter, slidingFitResult)).second)
                throw StatusCodeException(STATUS_CODE_FAILURE);
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------

void CosmicRayTrackMatchingAlgorithm::SelectMatchedTracks(const TwoDSlidingFitResultMap &slidingFitResultMap,
    const ClusterVector &clusterVector1, const ClusterVector &clusterVector2, const ClusterVector &clusterVector3,
    HitAssociationMap &hitAssociationMap, ClusterAssociationMap &clusterAssociationMap) const
{
    // Check that there are input clusters from all three views
    if (clusterVector1.empty() || clusterVector2.empty() || clusterVector3.empty())
        return;

    const HitType hitType1(LArThreeDHelper::GetClusterHitType(*clusterVector1.begin()));
    const HitType hitType2(LArThreeDHelper::GetClusterHitType(*clusterVector2.begin()));
    const HitType hitType3(LArThreeDHelper::GetClusterHitType(*clusterVector3.begin()));

    if (hitType1 == hitType2 || hitType2 == hitType3 || hitType3 == hitType1)
        return;

    // Loop over each pair of clusters and identify matches
    unsigned int clusterID(0);

    for (ClusterVector::const_iterator iter1 = clusterVector1.begin(), iterEnd1 = clusterVector1.end(); iter1 != iterEnd1; ++iter1)
    {
        Cluster* pCluster1 = *iter1;

        TwoDSlidingFitResultMap::const_iterator sIter1 = slidingFitResultMap.find(pCluster1);
        if (slidingFitResultMap.end() == sIter1)
            continue;

        const TwoDSlidingFitResult &slidingFitResult1(sIter1->second);

        for (ClusterVector::const_iterator iter2 = clusterVector2.begin(), iterEnd2 = clusterVector2.end(); iter2 != iterEnd2; ++iter2)
        {
            Cluster* pCluster2 = *iter2;

            TwoDSlidingFitResultMap::const_iterator sIter2 = slidingFitResultMap.find(pCluster2);
            if (slidingFitResultMap.end() == sIter2)
                continue;

            const TwoDSlidingFitResult &slidingFitResult2(sIter2->second);

            this->SelectMatchedTracks(++clusterID, slidingFitResult1, slidingFitResult2, clusterVector3,
                                      hitAssociationMap, clusterAssociationMap);
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------

void CosmicRayTrackMatchingAlgorithm::SelectMatchedTracks(const unsigned int clusterID, const TwoDSlidingFitResult &slidingFitResult1,
    const TwoDSlidingFitResult &slidingFitResult2, const ClusterVector &availableClusters, HitAssociationMap &hitAssociationMap,
    ClusterAssociationMap &clusterAssociationMap) const
{
    // Require a good X overlap between clusters
    const Cluster* pCluster1(slidingFitResult1.GetCluster());
    const Cluster* pCluster2(slidingFitResult2.GetCluster());

    float xMin1(0.f), xMax1(0.f), xMin2(0.f), xMax2(0.f);
    LArClusterHelper::GetClusterSpanX(pCluster1, xMin1, xMax1);
    LArClusterHelper::GetClusterSpanX(pCluster2, xMin2, xMax2);

    const float xOverlap(std::min(xMax1,xMax2) - std::max(xMin1,xMin2));
    const float xSpan(std::max(xMax1,xMax2) - std::min(xMin1,xMin2));

    if (xOverlap < m_minXOverlap || xOverlap/xSpan < m_minXOverlapFraction)
        return;


    // Sampling in x
    CartesianPointList projectedPositions;

    const HitType hitType1(LArThreeDHelper::GetClusterHitType(pCluster1));
    const HitType hitType2(LArThreeDHelper::GetClusterHitType(pCluster2));

    const float xMinSampling(std::max(xMin1,xMin2));
    const float xMaxSampling(std::min(xMax1,xMax2));

    const unsigned int nSamplingPoints(100); // <---- TODO: ADD TO CONFIGURATION

    for (unsigned int n = 0; n < nSamplingPoints; ++n)
    {
        const float alpha((0.5f + static_cast<float>(n)) / static_cast<float>(nSamplingPoints));
        const float x(xMinSampling + alpha * (xMaxSampling - xMinSampling));

        try
        {
            float chi2(0.f);
            CartesianVector position1(0.f, 0.f, 0.f), position2(0.f, 0.f, 0.f), position3(0.f, 0.f, 0.f);
            slidingFitResult1.GetGlobalFitPosition(x, true, position1);
            slidingFitResult2.GetGlobalFitPosition(x, true, position2);
            LArGeometryHelper::MergeTwoPositions(hitType1, hitType2, position1, position2, position3, chi2);
            projectedPositions.push_back(position3);

        }
        catch (StatusCodeException &)
        {
        }
    }

    if (projectedPositions.empty())
        return;


    // Find associated hits and clusters
    CaloHitList associatedHits;
    ClusterList associatedClusters;

    for(ClusterVector::const_iterator cIter = availableClusters.begin(), cIterEnd = availableClusters.end(); cIter != cIterEnd; ++cIter)
    {
        Cluster* pCluster = *cIter;

        bool isAssociatedCluster(false);

        CaloHitList availableCaloHits;
        pCluster->GetOrderedCaloHitList().GetCaloHitList(availableCaloHits);

        for (CaloHitList::const_iterator hIter = availableCaloHits.begin(), hIterEnd = availableCaloHits.end(); hIter != hIterEnd; ++hIter)
        {
            CaloHit *pCaloHit = *hIter;

            bool isAssociatedHit(false);

            for (CartesianPointList::const_iterator pIter = projectedPositions.begin(), pIterEnd = projectedPositions.end();
                pIter != pIterEnd; ++pIter)
            {
                const CartesianVector projectedPosition = *pIter;

                if ((pCaloHit->GetPositionVector() - projectedPosition).GetMagnitudeSquared() < m_maxPointDisplacement * m_maxPointDisplacement)
                {
                    isAssociatedHit = true;
                    isAssociatedCluster = true;
                    break;
                }
            }

            if (isAssociatedHit)
                associatedHits.insert(pCaloHit);
        }

        if (isAssociatedCluster)
            associatedClusters.insert(pCluster);
    }


    // Requirements on clusters
    bool goodClusters(true);

    for (ClusterList::const_iterator cIter = associatedClusters.begin(), cIterEnd = associatedClusters.end(); cIter != cIterEnd; ++cIter)
    {
        Cluster* pCluster = *cIter;

        float xMin(0.f), xMax(0.f);
        LArClusterHelper::GetClusterSpanX(pCluster, xMin, xMax);

        if ((xMax - xMin) > std::min((xMax1 - xMin1), (xMax2 - xMin2)))
        {
            goodClusters = false;
            break;
        }
    }

    if (!goodClusters)
        return;


    // Requirements on hits
    CaloHitList matchedHits;

    for (CaloHitList::const_iterator hIter1 = associatedHits.begin(), hIterEnd1 = associatedHits.end(); hIter1 != hIterEnd1; ++hIter1)
    {
        CaloHit* pCaloHit1 = *hIter1;

        bool isGoodHit(false);

        for (CaloHitList::const_iterator hIter2 = associatedHits.begin(), hIterEnd2 = associatedHits.end(); hIter2 != hIterEnd2; ++hIter2)
        {
            CaloHit* pCaloHit2 = *hIter2;

            if (pCaloHit1 == pCaloHit2)
                continue;

            if ((pCaloHit1->GetPositionVector() - pCaloHit2->GetPositionVector()).GetMagnitudeSquared() < m_maxHitDisplacement * m_maxHitDisplacement)
            {
                isGoodHit = true;
                break;
            }
        }

        if (isGoodHit)
            matchedHits.insert(pCaloHit1);
    }


    if (matchedHits.size() < m_minMatchedHits)
        return;


    // Requirements on points
    unsigned int nMatchedPoints(0);

    for (CartesianPointList::const_iterator pIter = projectedPositions.begin(), pIterEnd = projectedPositions.end(); pIter != pIterEnd; ++pIter)
    {
        const CartesianVector projectedPosition = *pIter;

        bool isAssociatedPoint(false);

        for (CaloHitList::const_iterator hIter = matchedHits.begin(), hIterEnd = matchedHits.end(); hIter != hIterEnd; ++hIter)
        {
            CaloHit* pCaloHit = *hIter;

            if ((pCaloHit->GetPositionVector() - projectedPosition).GetMagnitudeSquared() < m_maxPointDisplacement * m_maxPointDisplacement)
            {
                isAssociatedPoint = true;
                break;
            }
        }

        if (isAssociatedPoint)
            ++nMatchedPoints;
    }

    if (static_cast<float>(nMatchedPoints) / static_cast<float>(projectedPositions.size()) < m_minMatchedPointFraction)
        return;


    // Store the associations
    for (CaloHitList::const_iterator hIter = matchedHits.begin(), hIterEnd = matchedHits.end(); hIter != hIterEnd; ++hIter)
    {
        CaloHit *pCaloHit = *hIter;

        hitAssociationMap[pCaloHit].insert(clusterID);
        clusterAssociationMap[clusterID].insert(pCaloHit);
    }

// --- BEGIN EVENT DISPLAY ---
// ClusterList tempList1, tempList2, tempList3;
// tempList1.insert((Cluster*)pCluster1);
// tempList2.insert((Cluster*)pCluster2);
// tempList3.insert(availableClusters.begin(), availableClusters.end());
// PandoraMonitoringApi::SetEveDisplayParameters(false, DETECTOR_VIEW_XZ);
// PandoraMonitoringApi::VisualizeClusters(&tempList1, "Cluster1", RED);
// PandoraMonitoringApi::VisualizeClusters(&tempList2, "Cluster2", BLUE);
// PandoraMonitoringApi::VisualizeClusters(&tempList3, "Cluster3", GREEN);
// for (CartesianPointList::const_iterator tIter = projectedPositions.begin(), tIterEnd = projectedPositions.end(); tIter != tIterEnd; ++tIter)
// {
// CartesianVector tempPosition(*tIter);
// PandoraMonitoringApi::AddMarkerToVisualization(&tempPosition, "Projection", GREEN, 2.f);
// }
// PandoraMonitoringApi::ViewEvent();

// PandoraMonitoringApi::SetEveDisplayParameters(false, DETECTOR_VIEW_XZ);
// PandoraMonitoringApi::VisualizeClusters(&tempList1, "Cluster1", RED);
// PandoraMonitoringApi::VisualizeClusters(&tempList2, "Cluster2", BLUE);
// PandoraMonitoringApi::VisualizeCaloHits(&matchedHits, "AssociatedHits", GREEN);
// PandoraMonitoringApi::ViewEvent();
// --- END EVENT DISPLAY ---
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode CosmicRayTrackMatchingAlgorithm::ModifyClusters(const std::string inputClusterListName, HitAssociationMap &hitAssociationMap,
    ClusterAssociationMap &clusterAssociationMap) const
{
    // Reset the current cluster list for this view and get the available clusters
    const ClusterList *pCurrentClusterList = NULL;

    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::ReplaceCurrentList<Cluster>(*this, inputClusterListName));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::GetCurrentList<ClusterList>(*this, pCurrentClusterList));

    HitToClusterMap hitsToClusters;
    ClusterToHitMap clustersToHits;

    for (ClusterList::const_iterator cIter = pCurrentClusterList->begin(), cIterEnd = pCurrentClusterList->end(); cIter != cIterEnd; ++cIter)
    {
        Cluster *pCluster = *cIter;
        if (!pCluster->IsAvailable())
            continue;

        CaloHitList availableCaloHits;
        pCluster->GetOrderedCaloHitList().GetCaloHitList(availableCaloHits);

        for (CaloHitList::const_iterator hIter = availableCaloHits.begin(), hIterEnd = availableCaloHits.end(); hIter != hIterEnd; ++hIter)
        {
            CaloHit *pCaloHit = *hIter;
            hitsToClusters[pCaloHit].insert(pCluster);
            clustersToHits[pCluster].insert(pCaloHit);
        }
    }

    //Generate list of hits to form new clusters and remove from current clusters
    bool foundNewClusters(false);

    ClusterToHitMap clustersToModify;
    ClusterAssociationMap clustersToCreate;

    for (ClusterAssociationMap::const_iterator cIter = clusterAssociationMap.begin(), cIterEnd = clusterAssociationMap.end();
        cIter != cIterEnd; ++cIter)
    {
        unsigned int clusterID   = cIter->first;
        CaloHitList  caloHitList = cIter->second;

        for (CaloHitList::const_iterator hIter = caloHitList.begin(), hIterEnd = caloHitList.end(); hIter != hIterEnd; ++hIter)
        {
            CaloHit *pCaloHit = *hIter;

            if (hitAssociationMap[pCaloHit].size() > 1)
                continue;

            if (hitsToClusters[pCaloHit].size() > 1)
                return STATUS_CODE_FAILURE;

            ClusterList::const_iterator kIter = hitsToClusters[pCaloHit].begin();
            if (hitsToClusters[pCaloHit].end() == kIter)
                return STATUS_CODE_FAILURE;

            Cluster* pCluster = *kIter;

            clustersToModify[pCluster].insert(pCaloHit);
            clustersToCreate[clusterID].insert(pCaloHit);
            foundNewClusters = true;
        }
    }

    if (!foundNewClusters)
        return STATUS_CODE_SUCCESS;


    // Remove hits from current clusters
    for (ClusterToHitMap::const_iterator cIter = clustersToModify.begin(), cIterEnd = clustersToModify.end(); cIter != cIterEnd; ++cIter)
    {
        const Cluster* pClusterToRemove = cIter->first;
        CaloHitList caloHitsToRemove = cIter->second;

        ClusterToHitMap::const_iterator kIter = clustersToHits.find(pClusterToRemove);
        if (clustersToHits.end() == kIter)
            return STATUS_CODE_FAILURE;

        CaloHitList caloHitsToStart = kIter->second;
        CaloHitList caloHitsToKeep;

        for (CaloHitList::const_iterator hIter = caloHitsToStart.begin(), hIterEnd = caloHitsToStart.end(); hIter != hIterEnd; ++hIter)
        {
            CaloHit *pCaloHit = *hIter;
            if (caloHitsToRemove.count(pCaloHit) > 0)
                continue;

            caloHitsToKeep.insert(pCaloHit);
        }


        Cluster* pCluster = const_cast<Cluster*>(pClusterToRemove);

        if (caloHitsToKeep.empty())
        {
            PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::Delete<Cluster>(*this, pCluster));
        }
        else
        {
            for (CaloHitList::const_iterator hIter = caloHitsToRemove.begin(), hIterEnd = caloHitsToRemove.end(); hIter != hIterEnd; ++hIter)
            {
                CaloHit *pCaloHit = *hIter;
                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::RemoveFromCluster(*this, pCluster, pCaloHit));
            }
        }
    }

    // Create new clusters
    const ClusterList *pNewClusterList = NULL;
    std::string newClusterListName, currentClusterListName;

    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::GetCurrentListName<Cluster>(*this, currentClusterListName));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::CreateTemporaryListAndSetCurrent<ClusterList>(*this, pNewClusterList, newClusterListName));

    for (ClusterAssociationMap::const_iterator cIter = clustersToCreate.begin(), cIterEnd = clustersToCreate.end(); cIter != cIterEnd; ++cIter)
    {
        CaloHitList caloHitList = cIter->second;

        if (caloHitList.empty())
            return STATUS_CODE_FAILURE;

        PandoraContentApi::Cluster::Parameters newParameters;
        newParameters.m_caloHitList = caloHitList;

        Cluster *pNewCluster(NULL);
        PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::Cluster::Create(*this, newParameters, pNewCluster));
    }

    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraContentApi::SaveList<Cluster>(*this, newClusterListName, currentClusterListName));


// --- BEGIN EVENT DISPLAY ---
// ClusterVector newClusters;
// PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->GetAvailableClusters(inputClusterListName, newClusters));
// ClusterList tempList;
// tempList.insert(newClusters.begin(), newClusters.end());
// PandoraMonitoringApi::SetEveDisplayParameters(false, DETECTOR_VIEW_XZ);
// PandoraMonitoringApi::VisualizeClusters(&tempList, "NewClusters", BLACK);
// PandoraMonitoringApi::ViewEvent();
// --- END EVENT DISPLAY ---

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode CosmicRayTrackMatchingAlgorithm::ReadSettings(const TiXmlHandle xmlHandle)
{
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, XmlHelper::ReadValue(xmlHandle, "InputClusterListNameU", m_inputClusterListNameU));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, XmlHelper::ReadValue(xmlHandle, "InputClusterListNameV", m_inputClusterListNameV));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, XmlHelper::ReadValue(xmlHandle, "InputClusterListNameW", m_inputClusterListNameW));

    m_clusterMinLength = 10.f; // cm
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "ClusterMinLength", m_clusterMinLength));

    m_halfWindowLayers = 15;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "SlidingFitHalfWindow", m_halfWindowLayers));

    m_minXOverlap = 3.f;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MinXOverlap", m_minXOverlap));

    m_minXOverlapFraction = 0.8f;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MinXOverlapFraction", m_minXOverlapFraction));

    m_maxPointDisplacement = 1.5f;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MaxPointDisplacement", m_maxPointDisplacement));

    m_maxHitDisplacement = 5.f;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MaxHitDisplacement", m_maxHitDisplacement));

    m_minMatchedPointFraction = 0.8f;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MinMatchedPointraction", m_minMatchedPointFraction));

    m_minMatchedHits = 10;
    PANDORA_RETURN_RESULT_IF_AND_IF(STATUS_CODE_SUCCESS, STATUS_CODE_NOT_FOUND, !=, XmlHelper::ReadValue(xmlHandle,
        "MinMatchedHits", m_minMatchedHits));

    return STATUS_CODE_SUCCESS;
}

} // namespace lar
