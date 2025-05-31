#include "pch.h"

#include <list>
#include "PlanetPoseGenerator.h"
#include "DebugHelper.h"

using namespace DirectX;

//-----------------------------------------------------------------------------
// return true if the pose does not intersect anything in the universe
//-----------------------------------------------------------------------------
bool PlanetPoseGenerator::CheckFit(const Pos& pose,
    Layer* in_pLayer, float in_gap)
{
    for (auto& o : in_pLayer->m_poses)
    {
        float dist = XMVectorGetX(XMVector3LengthEst(o.pos - pose.pos));

        // leave a minimum spacing between planets
        if (dist < (o.radius + pose.radius + in_gap))
        {
            return false;
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// generate a random pose
//-----------------------------------------------------------------------------
float PlanetPoseGenerator::AddPose(Pos& pose, Layer* in_pLayer, Layer* pPreviousLayer)
{
    float minDistance = in_pLayer->minDistance;
    if (pPreviousLayer)
    {
        minDistance = pPreviousLayer->minDistance;
    }

    std::uniform_real_distribution<float> rDis(0, XM_2PI);
    std::uniform_real_distribution<float> xDis(minDistance, m_universeSize);

    XMVECTOR rotate = XMQuaternionIdentity();
    float distance{ 0 };
    bool fits = false;
    for (UINT i = 0; i < m_numTries; i++)
    {
        rotate = XMQuaternionRotationRollPitchYaw(rDis(m_gen), rDis(m_gen), rDis(m_gen));

        distance = xDis(m_gen);
        pose.pos = XMVector3Rotate(XMVectorSet(0, 0, distance, 1.f), rotate);

        fits = CheckFit(pose, in_pLayer, m_settings.gap);
        if (pPreviousLayer && fits)
        {
            fits = fits && CheckFit(pose, pPreviousLayer, m_settings.gap);
        }
        if (fits)
        {
            break;
        }
    }

    // if it doesn't fit, put it outside of the universe.
    if (!fits)
    {
        distance = m_universeSize + m_settings.gap + pose.radius;
        pose.pos = XMVector3Rotate(XMVectorSet(0, 0, distance, 1.f), rotate);
    }
    return distance;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
float PlanetPoseGenerator::GeneratePoses(std::vector<XMMATRIX>& out_matrices, std::vector<float>& out_radii)
{
    float range = m_settings.maxRadius - m_settings.minRadius;
    float midPoint = .5f * range;
    float stdDev = range / 5.f;
    std::normal_distribution<float>scaleDistrib(midPoint, stdDev);

    // expand min distance so no sphere can be too close
    float minDistance = m_settings.minDistance + m_settings.gap + m_settings.maxRadius;

    m_growSize = 3 * range;

    // start with a tiny universe
    m_universeSize = minDistance + m_growSize;

    std::list<Layer> layers(1);
    layers.back().minDistance = minDistance;
    layers.back().maxDistance = m_universeSize;

    Layer* pPreviousLayer = nullptr;
    Layer* pCurrentLayer = &layers.back();
    for (UINT n = 0; n < m_settings.numPoses; n++)
    {
        Pos pose;
        pose.radius = std::clamp(scaleDistrib(m_gen), m_settings.minRadius, m_settings.maxRadius);

        float distance = AddPose(pose, pCurrentLayer, pPreviousLayer);
        if (distance > pCurrentLayer->maxDistance) // universe grew
        {
            layers.push_back({});

            // the next layer minimum is sufficiently beyond the prior layer maximum that they cannot intersect
            minDistance = pCurrentLayer->maxDistance + m_settings.maxRadius + m_settings.gap;
            m_universeSize = minDistance + m_growSize;

            layers.back().minDistance = minDistance;
            layers.back().maxDistance = m_universeSize;

            pPreviousLayer = pCurrentLayer;
            pCurrentLayer = &layers.back();
        }
        pCurrentLayer->m_poses.push_back(pose);
    }

    // now have layers full of positions. Convert into matrices.
    std::uniform_real_distribution<float> rDis(0, XM_2PI);
    for (auto& layer : layers)
    {
        for (auto& pose : layer.m_poses)
        {
            out_radii.push_back(pose.radius);

            auto m =  XMMatrixTranslationFromVector(pose.pos);
            m.r[0] = XMVectorSet(pose.radius, 0, 0, 0);
            m.r[1] = XMVectorSet(0, pose.radius, 0, 0);
            m.r[2] = XMVectorSet(0, 0, pose.radius, 0);

            // spin each sphere in a random direction
            auto rotate = XMMatrixRotationRollPitchYaw(rDis(m_gen), rDis(m_gen), rDis(m_gen));
            out_matrices.push_back(rotate * m);
        }
	}

    return m_universeSize;
}
