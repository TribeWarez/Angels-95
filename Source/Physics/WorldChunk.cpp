#include "WorldChunk.hpp"
#include <algorithm>
#include <cmath>

void WorldChunkManager::Clear() {
    m_volumes.clear();
    m_cells.clear();
}

void WorldChunkManager::Build(const std::vector<Volume>& volumes, float chunkSize,
                              float worldMinX, float worldMaxX,
                              float worldMinZ, float worldMaxZ) {
    Clear();
    m_chunkSize = chunkSize;
    m_worldMinX = worldMinX;
    m_worldMaxX = worldMaxX;
    m_worldMinZ = worldMinZ;
    m_worldMaxZ = worldMaxZ;
    m_cellsX = (int)std::ceil((worldMaxX - worldMinX) / chunkSize);
    m_cellsZ = (int)std::ceil((worldMaxZ - worldMinZ) / chunkSize);
    if (m_cellsX < 1) m_cellsX = 1;
    if (m_cellsZ < 1) m_cellsZ = 1;

    m_volumes = volumes;

    // Assign each volume to all cells it overlaps
    for (int vi = 0; vi < (int)m_volumes.size(); vi++) {
        auto& v = m_volumes[vi];
        // Clamp volume bounds to world bounds to avoid out-of-range cells
        float cminX = std::max(v.minX, m_worldMinX);
        float cmaxX = std::min(v.maxX, m_worldMaxX);
        float cminZ = std::max(v.minZ, m_worldMinZ);
        float cmaxZ = std::min(v.maxZ, m_worldMaxZ);

        int ix0 = (int)std::floor((cminX - m_worldMinX) / m_chunkSize);
        int ix1 = (int)std::floor((cmaxX - m_worldMinX) / m_chunkSize);
        int iz0 = (int)std::floor((cminZ - m_worldMinZ) / m_chunkSize);
        int iz1 = (int)std::floor((cmaxZ - m_worldMinZ) / m_chunkSize);
        if (ix0 < 0) ix0 = 0; if (ix1 >= m_cellsX) ix1 = m_cellsX - 1;
        if (iz0 < 0) iz0 = 0; if (iz1 >= m_cellsZ) iz1 = m_cellsZ - 1;

        for (int iz = iz0; iz <= iz1; iz++) {
            for (int ix = ix0; ix <= ix1; ix++) {
                int idx = iz * m_cellsX + ix;
                m_cells[idx].push_back(vi);
            }
        }
    }
}

int WorldChunkManager::CellIndex(float worldX, float worldZ) const {
    int ix = (int)std::floor((worldX - m_worldMinX) / m_chunkSize);
    int iz = (int)std::floor((worldZ - m_worldMinZ) / m_chunkSize);
    if (ix < 0) ix = 0; if (ix >= m_cellsX) ix = m_cellsX - 1;
    if (iz < 0) iz = 0; if (iz >= m_cellsZ) iz = m_cellsZ - 1;
    return iz * m_cellsX + ix;
}

void WorldChunkManager::GetVolumesInCell(float worldX, float worldZ,
                                          std::vector<int>& outIndices) const {
    outIndices.clear();
    int idx = CellIndex(worldX, worldZ);
    auto it = m_cells.find(idx);
    if (it != m_cells.end())
        outIndices = it->second;
}

void WorldChunkManager::CollectCells(int ix0, int iz0, int ix1, int iz1,
                                      std::vector<int>& outIndices) const {
    for (int iz = iz0; iz <= iz1; iz++) {
        if (iz < 0 || iz >= m_cellsZ) continue;
        for (int ix = ix0; ix <= ix1; ix++) {
            if (ix < 0 || ix >= m_cellsX) continue;
            int idx = iz * m_cellsX + ix;
            auto it = m_cells.find(idx);
            if (it == m_cells.end()) continue;
            for (int vi : it->second) {
                // Avoid duplicates if a volume spans multiple cells
                if (std::find(outIndices.begin(), outIndices.end(), vi) == outIndices.end())
                    outIndices.push_back(vi);
            }
        }
    }
}

void WorldChunkManager::GetVolumesNear(float worldX, float worldZ,
                                        std::vector<int>& outIndices, int range) const {
    outIndices.clear();
    int cx = (int)std::floor((worldX - m_worldMinX) / m_chunkSize);
    int cz = (int)std::floor((worldZ - m_worldMinZ) / m_chunkSize);
    CollectCells(cx - range, cz - range, cx + range, cz + range, outIndices);
}

void WorldChunkManager::GetVolumesNearBox(float minX, float minZ, float maxX, float maxZ,
                                           std::vector<int>& outIndices, int range) const {
    outIndices.clear();
    // Normalise: a caller may hand us the corners in either order.
    if (minX > maxX) { float t = minX; minX = maxX; maxX = t; }
    if (minZ > maxZ) { float t = minZ; minZ = maxZ; maxZ = t; }

    // Every cell the box overlaps, plus `range` cells of slack.
    int ix0 = (int)std::floor((minX - m_worldMinX) / m_chunkSize) - range;
    int ix1 = (int)std::floor((maxX - m_worldMinX) / m_chunkSize) + range;
    int iz0 = (int)std::floor((minZ - m_worldMinZ) / m_chunkSize) - range;
    int iz1 = (int)std::floor((maxZ - m_worldMinZ) / m_chunkSize) + range;

    CollectCells(ix0, iz0, ix1, iz1, outIndices);
}
