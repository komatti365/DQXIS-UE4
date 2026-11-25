#include "DQXISAutoImportCommandlet.h"

#include "Animation/Skeleton.h"
#include "AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogDQXISAutoImport, Log, All);

UDQXISAutoImportCommandlet::UDQXISAutoImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

static bool SaveSinglePackage(UPackage* Pkg, UObject* Asset = nullptr)
{
	if (!Pkg) return false;
	const FString PkgName = Pkg->GetName();
	if (!PkgName.StartsWith(TEXT("/Game/"))) return false;
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PkgName, Filename, FPackageName::GetAssetPackageExtension()))
	{
		UE_LOG(LogDQXISAutoImport, Warning, TEXT("Could not resolve filename for package: %s"), *PkgName);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	const bool bSaved = GEditor->SavePackage(Pkg, Asset, RF_Public | RF_Standalone, *Filename, GWarn);
	UE_LOG(LogDQXISAutoImport, Display, TEXT("Saved package [%s]: %s"), bSaved ? TEXT("OK") : TEXT("FAIL"), *Filename);
	if (bSaved)
	{
		Pkg->SetDirtyFlag(false);
	}
	return bSaved;
}

static bool SaveAllDirtyContentPackages()
{
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
	bool bAllOk = true;
	for (UPackage* Pkg : DirtyPackages)
	{
		if (!Pkg) continue;
		const FString PkgName = Pkg->GetName();
		if (!PkgName.StartsWith(TEXT("/Game/"))) continue;
		if (!SaveSinglePackage(Pkg, nullptr))
		{
			bAllOk = false;
		}
	}
	return bAllOk;
}

/**
 * Ensures stub Master Material + MaterialInstanceConstant pair exists in the editor project
 * so our character MICs (MI_MMD_* / MI_MMDSKIN_*) inherit from DQXIS's native shaders with
 * ZERO compiled shader bytecode (bHasStaticPermutationResource == false).
 * The stubs live outside /Game/Characters/Human/<CharID>/ and are NEVER copied to 06_CookedOutput.
 */
static UMaterialInstanceConstant* EnsureStubMaterialInstance(
	const FString& MasterPkgName,
	const FString& MasterAssetName,
	const FString& MIPkgName,
	const FString& MIAssetName)
{
	const FString MasterObjPath = MasterPkgName + TEXT(".") + MasterAssetName;
	const FString MIObjPath     = MIPkgName + TEXT(".") + MIAssetName;

	UMaterial* BaseMat = LoadObject<UMaterial>(nullptr, *MasterObjPath);
	if (!BaseMat)
	{
		UPackage* MatPkg = CreatePackage(nullptr, *MasterPkgName);
		MatPkg->FullyLoad();
		BaseMat = NewObject<UMaterial>(MatPkg, FName(*MasterAssetName), RF_Public | RF_Standalone);

		UMaterialExpressionTextureSampleParameter2D* DiffParam =
			NewObject<UMaterialExpressionTextureSampleParameter2D>(BaseMat);
		DiffParam->ParameterName = FName(TEXT("DiffuseMap"));
		BaseMat->Expressions.Add(DiffParam);
		BaseMat->BaseColor.Expression = DiffParam;

		UMaterialExpressionScalarParameter* MetParam =
			NewObject<UMaterialExpressionScalarParameter>(BaseMat);
		MetParam->ParameterName = FName(TEXT("Metallic"));
		MetParam->DefaultValue = 0.0f;
		BaseMat->Expressions.Add(MetParam);
		BaseMat->Metallic.Expression = MetParam;

		auto AddStubScalarParam = [&](const TCHAR* ParamName, float DefaultVal) -> UMaterialExpressionScalarParameter*
		{
			UMaterialExpressionScalarParameter* P = NewObject<UMaterialExpressionScalarParameter>(BaseMat);
			P->ParameterName = FName(ParamName);
			P->DefaultValue = DefaultVal;
			BaseMat->Expressions.Add(P);
			return P;
		};
		BaseMat->Specular.Expression = AddStubScalarParam(TEXT("TEMP_SpecularMapScale"), 1.0f);
		BaseMat->Roughness.Expression = AddStubScalarParam(TEXT("TEMP_RoughnessMapScale"), 1.0f);
		BaseMat->AmbientOcclusion.Expression = AddStubScalarParam(TEXT("TEMP_DiffuseMapScale"), 1.0f);
		AddStubScalarParam(TEXT("RimScale"), 0.0f);
		AddStubScalarParam(TEXT("RimColorScale"), 1.0f);

		BaseMat->bUsedWithSkeletalMesh = true;
		BaseMat->PreEditChange(nullptr);
		BaseMat->PostEditChange();
		BaseMat->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BaseMat);
		SaveSinglePackage(MatPkg, BaseMat);
		UE_LOG(LogDQXISAutoImport, Display, TEXT("Created stub master %s"), *MasterPkgName);
	}

	UMaterialInstanceConstant* ExistingMI = LoadObject<UMaterialInstanceConstant>(nullptr, *MIObjPath);
	if (ExistingMI && ExistingMI->Parent)
	{
		return ExistingMI;
	}

	UPackage* MIPkg = CreatePackage(nullptr, *MIPkgName);
	MIPkg->FullyLoad();
	UMaterialInstanceConstant* BaseMI = NewObject<UMaterialInstanceConstant>(MIPkg, FName(*MIAssetName), RF_Public | RF_Standalone);
	BaseMI->SetParentEditorOnly(BaseMat);
	BaseMI->PreEditChange(nullptr);
	BaseMI->PostEditChange();
	BaseMI->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(BaseMI);
	SaveSinglePackage(MIPkg, BaseMI);
	UE_LOG(LogDQXISAutoImport, Display, TEXT("Created stub MI %s"), *MIPkgName);
	return BaseMI;
}

static USkeleton* EnsureHumanBaseSkeleton(const FString& SkeletonFbxPath)
{
	const FString SkelPkgName = TEXT("/Game/Characters/Human/S_Human_Base");
	const FString SkelObjPath = TEXT("/Game/Characters/Human/S_Human_Base.S_Human_Base");

	USkeleton* Existing = LoadObject<USkeleton>(nullptr, *SkelObjPath);
	if (Existing)
	{
		UE_LOG(LogDQXISAutoImport, Display, TEXT("Found existing S_Human_Base skeleton (%d bones)."),
			Existing->GetReferenceSkeleton().GetNum());
		return Existing;
	}

	UE_LOG(LogDQXISAutoImport, Display, TEXT("Creating S_Human_Base skeleton from: %s"), *SkeletonFbxPath);
	UFbxFactory* Factory = NewObject<UFbxFactory>();
	Factory->AddToRoot();
	UAutomatedAssetImportData* AutoData = NewObject<UAutomatedAssetImportData>();
	AutoData->bReplaceExisting = true;
	Factory->SetAutomatedAssetImportData(AutoData);
	Factory->SetDetectImportTypeOnImport(false);
	Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
	Factory->ImportUI->bImportAsSkeletal = true;
	Factory->ImportUI->bImportMesh = true;
	Factory->ImportUI->bImportMaterials = false;
	Factory->ImportUI->bImportTextures = false;
	Factory->ImportUI->bCreatePhysicsAsset = false;
	Factory->ImportUI->bAutomatedImportShouldDetectType = false;
	Factory->ImportUI->Skeleton = nullptr;

	FString TempPkgPath = TEXT("/Game/Characters/Human/_TempSkelBootstrap");
	FString AssetName = FPaths::GetBaseFilename(SkeletonFbxPath);
	UPackage* TempPkg = CreatePackage(nullptr, *(TempPkgPath + TEXT("/") + AssetName));
	bool bCanceled = false;
	UObject* Imported = Factory->ImportObject(
		USkeletalMesh::StaticClass(),
		TempPkg,
		FName(*AssetName),
		RF_Public | RF_Standalone,
		SkeletonFbxPath,
		nullptr,
		bCanceled
	);
	Factory->RemoveFromRoot();

	USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Imported);
	if (!SkelMesh || !SkelMesh->Skeleton)
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Failed to create skeleton from FBX: %s"), *SkeletonFbxPath);
		return nullptr;
	}

	UPackage* TargetPkg = CreatePackage(nullptr, *SkelPkgName);
	TargetPkg->FullyLoad();
	USkeleton* NewSkel = DuplicateObject<USkeleton>(SkelMesh->Skeleton, TargetPkg, TEXT("S_Human_Base"));
	NewSkel->SetFlags(RF_Public | RF_Standalone);
	NewSkel->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewSkel);
	SaveSinglePackage(TargetPkg, NewSkel);

	if (TempPkg) TempPkg->SetDirtyFlag(false);
	if (SkelMesh->Skeleton->GetOutermost()) SkelMesh->Skeleton->GetOutermost()->SetDirtyFlag(false);

	UE_LOG(LogDQXISAutoImport, Display, TEXT("Created and saved /Game/Characters/Human/S_Human_Base (%d bones)"),
		NewSkel->GetReferenceSkeleton().GetNum());
	return NewSkel;
}

static UTexture2D* ExtractPrimaryTextureFromMaterial(UMaterial* Mat)
{
	if (!Mat) return nullptr;

	if (Mat->BaseColor.Expression)
	{
		if (UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(Mat->BaseColor.Expression))
		{
			if (UTexture2D* Tex = Cast<UTexture2D>(TexExpr->Texture))
			{
				return Tex;
			}
		}
	}

	for (UMaterialExpression* Expr : Mat->Expressions)
	{
		if (UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(Expr))
		{
			if (UTexture2D* Tex = Cast<UTexture2D>(TexExpr->Texture))
			{
				return Tex;
			}
		}
	}
	return nullptr;
}

static bool ImportSkeletalFbxToPath(
	const FString& FbxFilePath,
	const FString& DestFolderPath,
	const FString& TargetAssetName,
	USkeleton* TargetSkeleton,
	UMaterialInstanceConstant* BaseCharaMI,
	UMaterialInstanceConstant* BaseSkinMI,
	bool bImportMaterials)
{
	if (!FPaths::FileExists(FbxFilePath))
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("FBX file does not exist: %s"), *FbxFilePath);
		return false;
	}

	UE_LOG(LogDQXISAutoImport, Display, TEXT("Importing FBX '%s' -> %s/%s (materials=%d)"),
		*FbxFilePath, *DestFolderPath, *TargetAssetName, bImportMaterials ? 1 : 0);

	UFbxFactory* Factory = NewObject<UFbxFactory>();
	Factory->AddToRoot();
	UAutomatedAssetImportData* AutoData = NewObject<UAutomatedAssetImportData>();
	AutoData->bReplaceExisting = true;
	Factory->SetAutomatedAssetImportData(AutoData);
	Factory->SetDetectImportTypeOnImport(false);
	Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
	Factory->ImportUI->bImportAsSkeletal = true;
	Factory->ImportUI->bImportMesh = true;
	Factory->ImportUI->bImportMaterials = bImportMaterials;
	Factory->ImportUI->bImportTextures = bImportMaterials;
	Factory->ImportUI->bCreatePhysicsAsset = false;
	Factory->ImportUI->bImportAnimations = false;
	Factory->ImportUI->bAutomatedImportShouldDetectType = false;
	Factory->ImportUI->Skeleton = TargetSkeleton;
	if (Factory->ImportUI->SkeletalMeshImportData)
	{
		Factory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = true;
		Factory->ImportUI->SkeletalMeshImportData->bUpdateSkeletonReferencePose = false;
		Factory->ImportUI->SkeletalMeshImportData->bUseT0AsRefPose = false;
	}

	const FString FullPkgPath = DestFolderPath + TEXT("/") + TargetAssetName;
	if (UPackage* ExistingPkg = FindPackage(nullptr, *FullPkgPath))
	{
		ResetLoaders(ExistingPkg);
		if (UObject* ExistingObj = StaticFindObjectFast(nullptr, ExistingPkg, FName(*TargetAssetName)))
		{
			ExistingObj->ClearFlags(RF_Public | RF_Standalone);
			ExistingObj->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders
			);
			ExistingObj->MarkPendingKill();
		}
	}

	FString ExistingFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(FullPkgPath, ExistingFilename, FPackageName::GetAssetPackageExtension()))
	{
		if (IFileManager::Get().FileExists(*ExistingFilename))
		{
			const bool bDeleted = IFileManager::Get().Delete(*ExistingFilename, false, true, true);
			UE_LOG(LogDQXISAutoImport, Display, TEXT("Deleted existing asset file before import [%s]: %s"),
				bDeleted ? TEXT("OK") : TEXT("FAIL"), *ExistingFilename);
		}
	}

	UPackage* Pkg = CreatePackage(nullptr, *FullPkgPath);
	if (UObject* Straggler = StaticFindObjectFast(nullptr, Pkg, FName(*TargetAssetName)))
	{
		Straggler->ClearFlags(RF_Public | RF_Standalone);
		Straggler->Rename(
			nullptr,
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders
		);
		Straggler->MarkPendingKill();
	}

	bool bCanceled = false;
	UObject* Imported = Factory->ImportObject(
		USkeletalMesh::StaticClass(),
		Pkg,
		FName(*TargetAssetName),
		RF_Public | RF_Standalone,
		FbxFilePath,
		nullptr,
		bCanceled
	);
	Factory->RemoveFromRoot();

	USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Imported);
	if (!SkelMesh)
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("FBX import returned null for %s"), *FbxFilePath);
		return false;
	}

	if (TargetSkeleton)
	{
		SkelMesh->Skeleton = TargetSkeleton;
	}

	int32 SlotIdx = 0;
	for (FSkeletalMaterial& MatSlot : SkelMesh->Materials)
	{
		SlotIdx++;
		if (bImportMaterials)
		{
			UMaterial* OrigMat = Cast<UMaterial>(MatSlot.MaterialInterface);
			UTexture2D* DiffuseTex = ExtractPrimaryTextureFromMaterial(OrigMat);

			FString RawName = OrigMat ? OrigMat->GetName() : FString::Printf(TEXT("MMD_%02d"), SlotIdx);
			const bool bIsSkin = RawName.StartsWith(TEXT("M_MMDSKIN")) || RawName.Contains(TEXT("SKIN"));
			UMaterialInstanceConstant* ParentMI = (bIsSkin && BaseSkinMI) ? BaseSkinMI : BaseCharaMI;

			FString MIName = RawName;
			if (MIName.StartsWith(TEXT("M_")))
			{
				MIName = TEXT("MI_") + MIName.Mid(2);
			}
			else if (!MIName.StartsWith(TEXT("MI_")))
			{
				MIName = TEXT("MI_") + MIName;
			}

			if (DiffuseTex)
			{
				DiffuseTex->SRGB = true;
				DiffuseTex->MarkPackageDirty();
			}

			const FString MIPkgPath = DestFolderPath + TEXT("/") + MIName;
			UPackage* MIPkg = CreatePackage(nullptr, *MIPkgPath);
			MIPkg->FullyLoad();

			UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
				MIPkg, FName(*MIName), RF_Public | RF_Standalone);
			MIC->SetParentEditorOnly(ParentMI);
			if (DiffuseTex)
			{
				MIC->SetTextureParameterValueEditorOnly(FName(TEXT("DiffuseMap")), DiffuseTex);
			}
			MIC->SetScalarParameterValueEditorOnly(FName(TEXT("Metallic")), 0.0f);
			// Anti-blowout tuning for bright MMD/anime albedo textures under DQXIS HDR lighting:
			// gently dampens diffuse multiplier, reduces harsh plastic specular glare, increases roughness,
			// and suppresses backlit rim-light clipping while keeping zero static shader permutations.
			MIC->SetScalarParameterValueEditorOnly(FName(TEXT("TEMP_DiffuseMapScale")), 0.94f);
			MIC->SetScalarParameterValueEditorOnly(FName(TEXT("TEMP_SpecularMapScale")), 0.35f);
			MIC->SetScalarParameterValueEditorOnly(FName(TEXT("TEMP_RoughnessMapScale")), 1.45f);
			if (!bIsSkin)
			{
				MIC->SetScalarParameterValueEditorOnly(FName(TEXT("RimScale")), 0.0f);
				MIC->SetScalarParameterValueEditorOnly(FName(TEXT("RimColorScale")), 0.25f);
			}
			// IMPORTANT: Do NOT override TwoSided/BlendMode on BasePropertyOverrides!
			// Keeping bHasStaticPermutationResource == false prevents compiling incompatible UE4 shaders.
			MIC->PreEditChange(nullptr);
			MIC->PostEditChange();
			MIC->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(MIC);

			MatSlot.MaterialInterface = MIC;

			if (OrigMat && OrigMat->GetOutermost())
			{
				OrigMat->GetOutermost()->SetDirtyFlag(false);
				OrigMat->ClearFlags(RF_Public | RF_Standalone);
			}

			UE_LOG(LogDQXISAutoImport, Display, TEXT("  Slot %02d -> %s (Parent=%s, DiffuseMap=%s)"),
				SlotIdx, *MIName, bIsSkin ? TEXT("MI_P_Chara_Base_Skin") : TEXT("MI_P_Chara_Base"),
				DiffuseTex ? *DiffuseTex->GetName() : TEXT("<default>"));
		}
		else
		{
			MatSlot.MaterialInterface = BaseCharaMI;
		}
	}

	SkelMesh->PostEditChange();
	SkelMesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(SkelMesh);
	return true;
}

int32 UDQXISAutoImportCommandlet::Main(const FString& Params)
{
	FString JobFilePath;
	if (!FParse::Value(*Params, TEXT("job="), JobFilePath))
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Missing -job=<path_to_json> parameter."));
		return 1;
	}
	JobFilePath = JobFilePath.TrimQuotes();

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *JobFilePath))
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Failed to read job file: %s"), *JobFilePath);
		return 1;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Invalid JSON in job file: %s"), *JobFilePath);
		return 1;
	}

	{
		TArray<UPackage*> StartupDirty;
		FEditorFileUtils::GetDirtyContentPackages(StartupDirty);
		for (UPackage* P : StartupDirty) P->SetDirtyFlag(false);
	}

	UMaterialInstanceConstant* BaseCharaMI = EnsureStubMaterialInstance(
		TEXT("/Game/Materials/BaseChara/M_Chara_Base"),
		TEXT("M_Chara_Base"),
		TEXT("/Game/Characters/Common/Material_Base/MI_P_Chara_Base"),
		TEXT("MI_P_Chara_Base")
	);
	UMaterialInstanceConstant* BaseSkinMI = EnsureStubMaterialInstance(
		TEXT("/Game/Materials/BaseChara/M_Chara_Base_Skin"),
		TEXT("M_Chara_Base_Skin"),
		TEXT("/Game/Characters/Common/Material_Base/MI_P_Chara_Base_Skin"),
		TEXT("MI_P_Chara_Base_Skin")
	);
	if (!BaseCharaMI || !BaseSkinMI)
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Failed to create/load stub base material instances."));
		return 2;
	}

	const FString SkelBootstrapFbx = Root->GetStringField(TEXT("skeleton_bootstrap_fbx"));
	USkeleton* BaseSkel = EnsureHumanBaseSkeleton(SkelBootstrapFbx);
	if (!BaseSkel)
	{
		return 2;
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("imports"), Items) || !Items)
	{
		UE_LOG(LogDQXISAutoImport, Error, TEXT("Job JSON missing 'imports' array."));
		return 3;
	}

	bool bAllOk = true;
	for (const TSharedPtr<FJsonValue>& Val : *Items)
	{
		const TSharedPtr<FJsonObject> Item = Val->AsObject();
		if (!Item.IsValid()) continue;
		const FString FbxPath = Item->GetStringField(TEXT("fbx_path"));
		const FString DestPath = Item->GetStringField(TEXT("dest_path"));
		const FString AssetName = Item->GetStringField(TEXT("asset_name"));
		bool bImportMats = true;
		Item->TryGetBoolField(TEXT("import_materials"), bImportMats);

		if (!ImportSkeletalFbxToPath(FbxPath, DestPath, AssetName, BaseSkel, BaseCharaMI, BaseSkinMI, bImportMats))
		{
			bAllOk = false;
		}
	}

	if (!SaveAllDirtyContentPackages())
	{
		UE_LOG(LogDQXISAutoImport, Warning, TEXT("Some packages reported save warnings."));
	}

	UE_LOG(LogDQXISAutoImport, Display, TEXT("DQXISAutoImport finished (ok=%d)."), bAllOk ? 1 : 0);
	return bAllOk ? 0 : 4;
}
