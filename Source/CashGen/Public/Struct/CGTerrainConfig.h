#pragma once

#include "CashGen/Public/Struct/CGLODConfig.h"
#include "CashGen/Public/WorldHeightInterface.h"

#include "CGTerrainConfig.generated.h"

/** Struct defines all applicable attributes for managing generation of a single zone */
USTRUCT(BlueprintType)
struct FCGTerrainConfig
{
	GENERATED_BODY()

	FCGTerrainConfig()
		:WaterMaterialInstance(nullptr)
	{
	}

	/** Noise Generator configuration struct */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tattiebogle|Data Source")
	TScriptInterface<IWorldHeightInterface> WorldHeightInterface;
	/** Use ASync collision cooking for terrain mesh (Recommended) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	bool UseAsyncCollision = true;
	/** Size of MeshData pool */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	uint8 MeshDataPoolSize = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	uint8 NumberOfThreads = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	uint8 MeshUpdatesPerFrame = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	FTimespan TileReleaseDelay;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|System")
	float TileSweepTime;
	/** Number of blocks along a zone's X axis */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Scale")
	int32 TileXUnits = 32;
	/** Number of blocks along a zone's Y axis */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Scale")
	int32 TileYUnits = 32;
	/** Size of a single block in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Scale")
	float UnitSize = 300.0f;
	/** Multiplier for heightmap*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Scale")
	float Amplitude = 5000.0f;

	/** Material for the terrain mesh */
	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	//UMaterial* TerrainMaterial;
	/** Material for the water mesh (will be instanced)*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	UMaterialInstance* WaterMaterialInstance;
	/** Cast Shadows */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	bool CastShadows = false;
	/* Generate a texture including heightmap and other information */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	bool GenerateSplatMap = false;
	/** If checked and numLODs > 1, material will be instanced and TerrainOpacity parameters used to dither LOD transitions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	bool DitheringLODTransitions = false;
	/** If no TerrainMaterial and LOD transitions disabled, just use the same static instance for all LODs **/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Rendering")
	UMaterialInstance* TerrainMaterialInstance;
	/** Make a dynamic material instance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Water")
	bool MakeDynamicMaterialInstance;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Water")
	/** If checked, will use a single instanced mesh for water, otherwise a procmesh section with dynamic texture will be used */
	bool UseInstancedWaterMesh = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Water")
	/** If checked, will use a single instanced mesh for water, otherwise a procmesh section with dynamic texture will be used */
	UStaticMesh* WaterMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|Water")
	TEnumAsByte<ECollisionEnabled::Type> WaterCollision = ECollisionEnabled::Type::QueryAndPhysics;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CashGen|LODs")
	TArray<FCGLODConfig> LODs;

	FVector TileOffset;
};
