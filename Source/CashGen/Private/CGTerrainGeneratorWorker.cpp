#include "CashGen/Public/CGTerrainGeneratorWorker.h"
#include "CashGen/Public/CGTile.h"

#include <ProceduralMeshComponent/Public/ProceduralMeshComponent.h>

#include <chrono>

DECLARE_CYCLE_STAT(TEXT("CashGenStat ~ HeightMap"), STAT_HeightMap, STATGROUP_CashGenStat);
DECLARE_CYCLE_STAT(TEXT("CashGenStat ~ Normals"), STAT_Normals, STATGROUP_CashGenStat);
DECLARE_CYCLE_STAT(TEXT("CashGenStat ~ Erosion"), STAT_Erosion, STATGROUP_CashGenStat);

FCGTerrainGeneratorWorker::FCGTerrainGeneratorWorker(ACGTerrainManager& aTerrainManager, FCGTerrainConfig& aTerrainConfig, TArray<TCGObjectPool<FCGMeshData>>& meshDataPoolPerLOD) 
	: pTerrainManager(aTerrainManager)
	, pTerrainConfig(aTerrainConfig)
	, pMeshDataPoolsPerLOD(meshDataPoolPerLOD)
{
}

FCGTerrainGeneratorWorker::~FCGTerrainGeneratorWorker()
{
}

bool FCGTerrainGeneratorWorker::Init()
{
	IsThreadFinished = false;
	return true;
}

uint32 FCGTerrainGeneratorWorker::Run()
{
	// Here's the loop
	while (!IsThreadFinished)
	{
		if (pTerrainManager.myPendingJobQueue.Dequeue(workJob))
		{
			workLOD = workJob.LOD;

			try
			{
				workJob.Data = pMeshDataPoolsPerLOD[workLOD].Borrow([&] { return !IsThreadFinished; });
			}
			catch (const std::exception&)
			{
				if (IsThreadFinished)
				{
					// seems borrowing aborted because IsThreadFinished got true. Let's just return
					return 1;
				}
				// and in any other case, rethrow
				throw;
			}

			pMeshData = workJob.Data.Get();

			std::chrono::milliseconds startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch());

			prepMaps();
			ProcessTerrainMap();

			workJob.HeightmapGenerationDuration = (std::chrono::duration_cast<std::chrono::milliseconds>(
													   std::chrono::system_clock::now().time_since_epoch()) -
												   startMs)
													  .count();

			ProcessPerBlockGeometry();
			ProcessPerVertexTasks();
			ProcessSkirtGeometry();

			pTerrainManager.myUpdateJobQueue.Enqueue(workJob);
		}
		// Otherwise, take a nap
		else
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}

	return 1;
}

void FCGTerrainGeneratorWorker::Stop()
{
	IsThreadFinished = true;
}

void FCGTerrainGeneratorWorker::Exit()
{
}

void FCGTerrainGeneratorWorker::prepMaps()
{
	// TODO : VERTEX COLORS
	for (int32 i = 0; i < pMeshData->MyColours.Num(); ++i)
	{
		pMeshData->MyColours[i].R = 0;
		pMeshData->MyColours[i].G = 0;
		pMeshData->MyColours[i].B = 0;
		pMeshData->MyColours[i].A = 0;
	}
}

void FCGTerrainGeneratorWorker::ProcessTerrainMap()
{
	SCOPE_CYCLE_COUNTER(STAT_HeightMap);
	// Size of the noise sampling (larger than the actual mesh so we can have seamless normals)
	int32 exX = GetNumberOfNoiseSamplePoints();
	int32 exY = exX;

	const int32 XYunits = workLOD == 0 ? pTerrainConfig.TileXUnits : pTerrainConfig.TileXUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor;
	const int32 exUnitSize = workLOD == 0 ? pTerrainConfig.UnitSize : pTerrainConfig.UnitSize * pTerrainConfig.LODs[workLOD].ResolutionDivisor;

	UObject* WorldInterfaceObject = pTerrainConfig.WorldHeightInterface.GetObject();
	// Calculate the new noisemap
	for (int y = 0; y < exY; ++y)
	{
		for (int x = 0; x < exX; ++x)
		{
			int32 worldX = (((workJob.mySector.X * XYunits) + x) * exUnitSize) - exUnitSize;
			int32 worldY = (((workJob.mySector.Y * XYunits) + y) * exUnitSize) - exUnitSize;

			pMeshData->HeightMap[x + (exX * y)] = IWorldHeightInterface::Execute_GetHeightAtPoint(WorldInterfaceObject, worldX, worldY);
		}
	}
	// Put heightmap into Red channel

	if (pTerrainConfig.GenerateSplatMap && workLOD == 0)
	{
		int i = 0;
		for (int y = 0; y < pTerrainConfig.TileYUnits; ++y)
		{
			for (int x = 0; x < pTerrainConfig.TileXUnits; ++x)
			{
				float& noiseValue = pMeshData->HeightMap[(x + 1) + (exX * (y + 1))];

				pMeshData->myTextureData[i].R = (uint8)FMath::GetMappedRangeValueClamped(FVector2D(0.0f, 1.0f), FVector2D(0.0f, 255.0f), noiseValue);

				pMeshData->myTextureData[i].G = (uint8)FMath::GetMappedRangeValueClamped(FVector2D(-1.0f, 0.0f), FVector2D(0.0f, 255.0f), noiseValue);

				pMeshData->myTextureData[i].B = i;

				pMeshData->myTextureData[i].A = 0;

				i++;
			}
		}
	}

	// Then put the biome map into the Green vertex colour channel
	if (pTerrainConfig.BiomeBlendGenerator)
	{
		exX -= 2;
		exY -= 2;
		for (int y = 0; y < exY; ++y)
		{
			for (int x = 0; x < exX; ++x)
			{
				int32 worldX = (((workJob.mySector.X * (exX - 1)) + x) * exUnitSize);
				int32 worldY = (((workJob.mySector.Y * (exX - 1)) + y) * exUnitSize);
				float val = pTerrainConfig.BiomeBlendGenerator->GetNoise2D(worldX, worldY);

				pMeshData->MyColours[x + (exX * y)].G = FMath::Clamp(FMath::RoundToInt(((val + 1.0f) / 2.0f) * 256), 0, 255);
			}
		}
	}
}

void FCGTerrainGeneratorWorker::ProcessPerBlockGeometry()
{
	int32 vertCounter = 0;
	int32 triCounter = 0;

	int32 xUnits = workLOD == 0 ? pTerrainConfig.TileXUnits : (pTerrainConfig.TileXUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor);
	int32 yUnits = workLOD == 0 ? pTerrainConfig.TileYUnits : (pTerrainConfig.TileYUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor);

	// Generate the mesh data for each block
	for (int32 y = 0; y < yUnits; ++y)
	{
		for (int32 x = 0; x < xUnits; ++x)
		{
			UpdateOneBlockGeometry(x, y, vertCounter, triCounter);
		}
	}
}

void FCGTerrainGeneratorWorker::ProcessPerVertexTasks()
{
	SCOPE_CYCLE_COUNTER(STAT_Normals);
	int32 xUnits = workLOD == 0 ? pTerrainConfig.TileXUnits : (pTerrainConfig.TileXUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor);
	int32 yUnits = workLOD == 0 ? pTerrainConfig.TileYUnits : (pTerrainConfig.TileYUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor);

	int32 rowLength = workLOD == 0 ? pTerrainConfig.TileXUnits + 1 : (pTerrainConfig.TileXUnits / (pTerrainConfig.LODs[workLOD].ResolutionDivisor) + 1);

	for (int32 y = 0; y < yUnits + 1; ++y)
	{
		for (int32 x = 0; x < xUnits + 1; ++x)
		{
			FVector normal;
			FProcMeshTangent tangent(0.0f, 1.0f, 0.f);

			GetNormalFromHeightMapForVertex(x, y, normal);

			uint8 slopeChan = FMath::RoundToInt((1.0f - FMath::Abs(FVector::DotProduct(normal, FVector::UpVector))) * 256);
			pMeshData->MyColours[x + (y * rowLength)].R = slopeChan;
			pMeshData->MyNormals[x + (y * rowLength)] = normal;
			pMeshData->MyTangents[x + (y * rowLength)] = tangent;
		}
	}
}

// Generates the 'skirt' geometry that falls down from the edges of each tile
void FCGTerrainGeneratorWorker::ProcessSkirtGeometry()
{
	// Going to do this the simple way, keep code easy to understand!

	int32 numXVerts = workLOD == 0 ? pTerrainConfig.TileXUnits + 1 : (pTerrainConfig.TileXUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor) + 1;
	int32 numYVerts = workLOD == 0 ? pTerrainConfig.TileYUnits + 1 : (pTerrainConfig.TileYUnits / pTerrainConfig.LODs[workLOD].ResolutionDivisor) + 1;

	int32 startIndex = numXVerts * numYVerts;
	int32 triStartIndex = ((numXVerts - 1) * (numYVerts - 1) * 6);

	// Bottom Edge verts
	for (int i = 0; i < numXVerts; ++i)
	{
		pMeshData->MyPositions[startIndex + i].X = pMeshData->MyPositions[i].X;
		pMeshData->MyPositions[startIndex + i].Y = pMeshData->MyPositions[i].Y;
		pMeshData->MyPositions[startIndex + i].Z = -30000.0f;

		pMeshData->MyNormals[startIndex + i] = pMeshData->MyNormals[i];
	}
	// bottom edge triangles
	for (int i = 0; i < ((numXVerts - 1)); ++i)
	{
		pMeshData->MyTriangles[triStartIndex + (i * 6)] = i;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 1] = startIndex + i + 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 2] = startIndex + i;

		pMeshData->MyTriangles[triStartIndex + (i * 6) + 3] = i + 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 4] = startIndex + i + 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 5] = i;
	}
	triStartIndex += ((numXVerts - 1) * 6);

	startIndex = ((numXVerts) * (numYVerts + 1));
	// Top Edge verts
	for (int i = 0; i < numXVerts; ++i)
	{
		pMeshData->MyPositions[startIndex + i].X = pMeshData->MyPositions[i + startIndex - (numXVerts * 2)].X;
		pMeshData->MyPositions[startIndex + i].Y = pMeshData->MyPositions[i + startIndex - (numXVerts * 2)].Y;
		pMeshData->MyPositions[startIndex + i].Z = -30000.0f;

		pMeshData->MyNormals[startIndex + i] = pMeshData->MyNormals[i + startIndex - (numXVerts * 2)];
	}
	// top edge triangles

	for (int i = 0; i < ((numXVerts - 1)); ++i)
	{
		pMeshData->MyTriangles[triStartIndex + (i * 6)] = i + startIndex - (numXVerts * 2);
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 1] = startIndex + i;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 2] = i + startIndex - (numXVerts * 2) + 1;

		pMeshData->MyTriangles[triStartIndex + (i * 6) + 3] = i + startIndex - (numXVerts * 2) + 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 4] = startIndex + i;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 5] = startIndex + i + 1;
	}
	triStartIndex += ((numXVerts - 1) * 6);

	startIndex = numXVerts * (numYVerts + 2);
	// Right edge - bit different
	for (int i = 0; i < numYVerts - 2; ++i)
	{
		pMeshData->MyPositions[startIndex + i].X = pMeshData->MyPositions[(i + 1) * numXVerts].X;
		pMeshData->MyPositions[startIndex + i].Y = pMeshData->MyPositions[(i + 1) * numXVerts].Y;
		pMeshData->MyPositions[startIndex + i].Z = -30000.0f;

		pMeshData->MyNormals[startIndex + i] = pMeshData->MyNormals[(i + 1) * numXVerts];
	}
	// Bottom right corner

	pMeshData->MyTriangles[triStartIndex] = 0;
	pMeshData->MyTriangles[triStartIndex + 1] = numXVerts * numYVerts;
	pMeshData->MyTriangles[triStartIndex + 2] = numXVerts;

	pMeshData->MyTriangles[triStartIndex + 3] = numXVerts;
	pMeshData->MyTriangles[triStartIndex + 4] = numXVerts * numYVerts;
	pMeshData->MyTriangles[triStartIndex + 5] = numXVerts * (numYVerts + 2);

	// Top right corner
	triStartIndex += 6;

	pMeshData->MyTriangles[triStartIndex] = numXVerts * (numYVerts - 1);
	pMeshData->MyTriangles[triStartIndex + 1] = (numXVerts * (numYVerts + 2)) + numYVerts - 3;
	pMeshData->MyTriangles[triStartIndex + 2] = numXVerts * (numYVerts + 1);

	pMeshData->MyTriangles[triStartIndex + 3] = numXVerts * (numYVerts - 1);
	pMeshData->MyTriangles[triStartIndex + 4] = numXVerts * (numYVerts - 2);
	pMeshData->MyTriangles[triStartIndex + 5] = (numXVerts * (numYVerts + 2)) + numYVerts - 3;

	// Middle right part!
	startIndex = numXVerts * (numYVerts + 2);
	triStartIndex += 6;

	for (int i = 0; i < numYVerts - 3; ++i)
	{
		pMeshData->MyTriangles[triStartIndex + (i * 6)] = numXVerts * (i + 1);
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 1] = startIndex + i;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 2] = numXVerts * (i + 2);

		pMeshData->MyTriangles[triStartIndex + (i * 6) + 3] = numXVerts * (i + 2);
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 4] = startIndex + i;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 5] = startIndex + i + 1;
	}
	triStartIndex += ((numYVerts - 3) * 6);

	startIndex += (numYVerts - 2);
	// Left edge - bit different
	for (int i = 0; i < numYVerts - 2; ++i)
	{
		pMeshData->MyPositions[startIndex + i].X = pMeshData->MyPositions[((i + 1) * numXVerts) + numXVerts - 1].X;
		pMeshData->MyPositions[startIndex + i].Y = pMeshData->MyPositions[((i + 1) * numXVerts) + numXVerts - 1].Y;
		pMeshData->MyPositions[startIndex + i].Z = -30000.0f;

		pMeshData->MyNormals[startIndex + i] = pMeshData->MyNormals[((i + 1) * numXVerts) + numXVerts - 1];
	}
	// Bottom left corner

	pMeshData->MyTriangles[triStartIndex] = numXVerts - 1;
	pMeshData->MyTriangles[triStartIndex + 1] = (numXVerts * 2) - 1;
	pMeshData->MyTriangles[triStartIndex + 2] = startIndex;

	pMeshData->MyTriangles[triStartIndex + 3] = startIndex;
	pMeshData->MyTriangles[triStartIndex + 4] = (numXVerts * numYVerts) + numXVerts - 1;
	pMeshData->MyTriangles[triStartIndex + 5] = numXVerts - 1;

	// Top left corner
	triStartIndex += 6;

	pMeshData->MyTriangles[triStartIndex] = (numXVerts * numYVerts) - 1;
	pMeshData->MyTriangles[triStartIndex + 1] = (numXVerts * (numYVerts + 2)) - 1;
	pMeshData->MyTriangles[triStartIndex + 2] = (numXVerts * (numYVerts + 2)) + ((numYVerts - 2) * 2) - 1;

	pMeshData->MyTriangles[triStartIndex + 3] = (numXVerts * numYVerts) - 1;
	pMeshData->MyTriangles[triStartIndex + 4] = (numXVerts * (numYVerts + 2)) + ((numYVerts - 2) * 2) - 1;
	pMeshData->MyTriangles[triStartIndex + 5] = (numXVerts * (numYVerts - 2)) + numXVerts - 1;

	// Middle left part!

	triStartIndex += 6;

	for (int i = 0; i < numYVerts - 3; ++i)
	{
		pMeshData->MyTriangles[triStartIndex + (i * 6)] = (numXVerts * (i + 1)) + numXVerts - 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 1] = (numXVerts * (i + 2)) + numXVerts - 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 2] = startIndex + i + 1;

		pMeshData->MyTriangles[triStartIndex + (i * 6) + 3] = (numXVerts * (i + 1)) + numXVerts - 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 4] = startIndex + i + 1;
		pMeshData->MyTriangles[triStartIndex + (i * 6) + 5] = startIndex + i;
	}
}

void FCGTerrainGeneratorWorker::GetNormalFromHeightMapForVertex(const int32& vertexX, const int32& vertexY, FVector& aOutNormal) //, FVector& aOutTangent)
{
	FVector result;

	FVector tangentVec, bitangentVec;

	const int32 rowLength = workLOD == 0 ? pTerrainConfig.TileXUnits + 1 : (pTerrainConfig.TileXUnits / (pTerrainConfig.LODs[workLOD].ResolutionDivisor) + 1);
	const int32 heightMapRowLength = rowLength + 2;

	// the heightmapIndex for this vertex index
	const int32 heightMapIndex = vertexX + 1 + ((vertexY + 1) * heightMapRowLength);
	const float worldTileX = workJob.mySector.X * pTerrainConfig.TileXUnits;
	const float worldTileY = workJob.mySector.Y * pTerrainConfig.TileYUnits;
	const float& unitSize = workLOD == 0 ? pTerrainConfig.UnitSize : pTerrainConfig.UnitSize * pTerrainConfig.LODs[workLOD].ResolutionDivisor;
	const float& ampl = pTerrainConfig.Amplitude;

	FVector origin = FVector((worldTileX + vertexX) * unitSize, (worldTileY + vertexY) * unitSize, pMeshData->HeightMap[heightMapIndex] * ampl);

	// Get the 4 neighbouring points
	FVector up, down, left, right;

	up = FVector((worldTileX + vertexX) * unitSize, (worldTileY + vertexY + 1) * unitSize, pMeshData->HeightMap[heightMapIndex + heightMapRowLength] * ampl) - origin;
	down = FVector((worldTileX + vertexX) * unitSize, (worldTileY + vertexY - 1) * unitSize, pMeshData->HeightMap[heightMapIndex - heightMapRowLength] * ampl) - origin;
	left = FVector((worldTileX + vertexX + 1) * unitSize, (worldTileY + vertexY) * unitSize, pMeshData->HeightMap[heightMapIndex + 1] * ampl) - origin;
	right = FVector((worldTileX + vertexX - 1) * unitSize, (worldTileY + vertexY) * unitSize, pMeshData->HeightMap[heightMapIndex - 1] * ampl) - origin;

	FVector n1, n2, n3, n4;

	n1 = FVector::CrossProduct(left, up);
	n2 = FVector::CrossProduct(up, right);
	n3 = FVector::CrossProduct(right, down);
	n4 = FVector::CrossProduct(down, left);

	result = n1 + n2 + n3 + n4;

	aOutNormal = result.GetSafeNormal();

	// We can mega cheap out here as we're dealing with a simple flat grid
	//aOutTangent = FRuntimeMeshTangent(left.GetSafeNormal(), false);
}

void FCGTerrainGeneratorWorker::UpdateOneBlockGeometry(const int32& aX, const int32& aY, int32& aVertCounter, int32& triCounter)
{
	int32 thisX = aX;
	int32 thisY = aY;
	int32 heightMapX = thisX + 1;
	int32 heightMapY = thisY + 1;
	// LOD adjusted dimensions
	int32 rowLength = workLOD == 0 ? pTerrainConfig.TileXUnits + 1 : (pTerrainConfig.TileXUnits / (pTerrainConfig.LODs[workLOD].ResolutionDivisor) + 1);
	int32 heightMapRowLength = rowLength + 2;
	// LOD adjusted unit size
	int32 exUnitSize = workLOD == 0 ? pTerrainConfig.UnitSize : pTerrainConfig.UnitSize * (pTerrainConfig.LODs[workLOD].ResolutionDivisor);

	const int blockX = 0;
	const int blockY = 0;
	const float& unitSize = pTerrainConfig.UnitSize;
	const float& ampl = pTerrainConfig.Amplitude;

	FVector heightMapToWorldOffset = FVector(0.0f, 0.0f, 0.0f);

	// TL
	pMeshData->MyPositions[thisX + (thisY * rowLength)] = FVector((blockX + thisX) * exUnitSize, (blockY + thisY) * exUnitSize, pMeshData->HeightMap[heightMapX + (heightMapY * heightMapRowLength)] * ampl) - heightMapToWorldOffset;
	// TR
	pMeshData->MyPositions[thisX + ((thisY + 1) * rowLength)] = FVector((blockX + thisX) * exUnitSize, (blockY + thisY + 1) * exUnitSize, pMeshData->HeightMap[heightMapX + ((heightMapY + 1) * heightMapRowLength)] * ampl) - heightMapToWorldOffset;
	// BL
	pMeshData->MyPositions[(thisX + 1) + (thisY * rowLength)] = FVector((blockX + thisX + 1) * exUnitSize, (blockY + thisY) * exUnitSize, pMeshData->HeightMap[(heightMapX + 1) + (heightMapY * heightMapRowLength)] * ampl) - heightMapToWorldOffset;
	// BR
	pMeshData->MyPositions[(thisX + 1) + ((thisY + 1) * rowLength)] = FVector((blockX + thisX + 1) * exUnitSize, (blockY + thisY + 1) * exUnitSize, pMeshData->HeightMap[(heightMapX + 1) + ((heightMapY + 1) * heightMapRowLength)] * ampl) - heightMapToWorldOffset;
}

int32 FCGTerrainGeneratorWorker::GetNumberOfNoiseSamplePoints()
{
	return workLOD == 0 ? pTerrainConfig.TileXUnits + 3 : (pTerrainConfig.TileXUnits / (pTerrainConfig.LODs[workLOD].ResolutionDivisor)) + 3;
}
