// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelSnapshotsEditorModule.h"

#include "Settings/LevelSnapshotsEditorProjectSettings.h"
#include "Settings/LevelSnapshotsEditorDataManagementSettings.h"
#include "NegatableFilter.h"
#include "LevelSnapshotsEditorCommands.h"
#include "LevelSnapshotsEditorStyle.h"
#include "LevelSnapshotsEditorFunctionLibrary.h"
#include "LevelSnapshotsEditorData.h"
#include "LevelSnapshotsFunctionLibrary.h"
#include "LevelSnapshotsUserSettings.h"
#include "SLevelSnapshotsEditorCreationForm.h"
#include "Toolkits/LevelSnapshotsEditorToolkit.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetTypeActions/AssetTypeActions_LevelSnapshot.h"
#include "IAssetTools.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "FLevelSnapshotsEditorModule"

FLevelSnapshotsEditorModule& FLevelSnapshotsEditorModule::Get()
{
	return FModuleManager::GetModuleChecked<FLevelSnapshotsEditorModule>("LevelSnapshotsEditor");
}

void FLevelSnapshotsEditorModule::StartupModule()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	AssetTools.RegisterAssetTypeActions(MakeShared<FAssetTypeActions_LevelSnapshot>());

	FLevelSnapshotsEditorStyle::Initialize();
	FLevelSnapshotsEditorCommands::Register();
	
	// add the menu subsection
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FLevelSnapshotsEditorModule::PostEngineInit);
}

void FLevelSnapshotsEditorModule::ShutdownModule()
{
	UToolMenus::UnregisterOwner(this);

	FCoreDelegates::OnPostEngineInit.RemoveAll(this);

	FLevelSnapshotsEditorStyle::Shutdown();

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.UnregisterCustomClassLayout(UNegatableFilter::StaticClass()->GetFName());
	
	FLevelSnapshotsEditorCommands::Unregister();

	// Unregister project settings
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");
	{
		SettingsModule.UnregisterSettings("Project", "Plugins", "Level Snapshots");
		SettingsModule.UnregisterSettings("Project", "Plugins", "Level Snapshots Data Management");
	}
}

bool FLevelSnapshotsEditorModule::GetUseCreationForm() const
{
	if (ensureMsgf(ProjectSettingsObjectPtr.IsValid(), 
		TEXT("ProjectSettingsObjectPtr was not valid. Returning false for bUseCreationForm. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		return ProjectSettingsObjectPtr.Get()->bUseCreationForm;
	}
	
	return false;
}

void FLevelSnapshotsEditorModule::SetUseCreationForm(bool bInUseCreationForm)
{
	if (ensureMsgf(ProjectSettingsObjectPtr.IsValid(),
		TEXT("ProjectSettingsObjectPtr was not valid. Returning false for bUseCreationForm. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		ProjectSettingsObjectPtr.Get()->bUseCreationForm = bInUseCreationForm;
	}
}

void FLevelSnapshotsEditorModule::PostEngineInit()
{
	RegisterMenuItem();

	if (RegisterProjectSettings() && ProjectSettingsObjectPtr->bEnableLevelSnapshotsToolbarButton)
	{
		RegisterEditorToolbar();
	}
}

void FLevelSnapshotsEditorModule::RegisterMenuItem()
{
	if (FSlateApplication::IsInitialized())
	{
		if (IsRunningGame())
		{
			return;
		}
		
		TSharedRef<FUICommandList> MenuItemCommandList = MakeShareable(new FUICommandList);

		MenuItemCommandList->MapAction(
			FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorMenuItem,
			FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::OpenSnapshotsEditor)
		);
		
		TSharedPtr<FExtender> NewMenuExtender = MakeShareable(new FExtender);
		NewMenuExtender->AddMenuExtension("ExperimentalTabSpawners", 
		                                  EExtensionHook::After, 
		                                  MenuItemCommandList, 
		                                  FMenuExtensionDelegate::CreateLambda([this] (FMenuBuilder& MenuBuilder)
		                                  {
			                                  MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorMenuItem);
		                                  }));
	
		// Get the Level Editor so we can insert our item into the Level Editor menu subsection
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");

		LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(NewMenuExtender);
	}
}

bool FLevelSnapshotsEditorModule::RegisterProjectSettings()
{
	ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");
	{
		// User Project Settings
		ProjectSettingsSectionPtr = SettingsModule.RegisterSettings("Project", "Plugins", "Level Snapshots",
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsSettingsCategoryDisplayName", "Level Snapshots"),
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsSettingsDescription", "Configure the Level Snapshots user settings"),
			GetMutableDefault<ULevelSnapshotsEditorProjectSettings>());

		if (ProjectSettingsSectionPtr.IsValid() && ProjectSettingsSectionPtr->GetSettingsObject().IsValid())
		{
			ProjectSettingsObjectPtr = Cast<ULevelSnapshotsEditorProjectSettings>(ProjectSettingsSectionPtr->GetSettingsObject());

			ProjectSettingsSectionPtr->OnModified().BindRaw(this, &FLevelSnapshotsEditorModule::HandleModifiedProjectSettings);
		}

		// Data Management Project Settings
		DataMangementSettingsSectionPtr = SettingsModule.RegisterSettings("Project", "Plugins", "Level Snapshots Data Management",
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsDataManagementSettingsCategoryDisplayName", "Level Snapshots Data Management"),
			NSLOCTEXT("LevelSnapshots", "LevelSnapshotsDataManagementSettingsDescription", "Configure the Level Snapshots path and data settings"),
			GetMutableDefault<ULevelSnapshotsEditorDataManagementSettings>());

		if (DataMangementSettingsSectionPtr.IsValid() && DataMangementSettingsSectionPtr->GetSettingsObject().IsValid())
		{
			DataMangementSettingsObjectPtr = Cast<ULevelSnapshotsEditorDataManagementSettings>(DataMangementSettingsSectionPtr->GetSettingsObject());

			DataMangementSettingsSectionPtr->OnModified().BindRaw(this, &FLevelSnapshotsEditorModule::HandleModifiedProjectSettings);
		}
	}

	return ProjectSettingsObjectPtr.IsValid();
}

bool FLevelSnapshotsEditorModule::HandleModifiedProjectSettings()
{
	if (ensureMsgf(DataMangementSettingsObjectPtr.IsValid(),
		TEXT("ProjectSettingsObjectPtr was not valid. Check to ensure that Project Settings have been registered for LevelSnapshots.")))
	{
		DataMangementSettingsObjectPtr->ValidateRootLevelSnapshotSaveDirAsGameContentRelative();
		DataMangementSettingsObjectPtr->SanitizeAllProjectSettingsPaths(true);
		
		DataMangementSettingsObjectPtr.Get()->SaveConfig();
	}
	
	return true;
}

void FLevelSnapshotsEditorModule::RegisterEditorToolbar()
{
	if (IsRunningGame())
	{
		return;
	}
	
	// Get the Level Editor so we can insert our combo button into the Level Editor's toolbar
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	
	MapEditorToolbarActions();

	// Create a toolbar extension instance that will insert our toolbar button after the 'Settings' horizontal box in the toolbar
	TSharedPtr<FExtender> NewToolbarExtender = MakeShareable(new FExtender);
	NewToolbarExtender->AddToolBarExtension("Settings",
		EExtensionHook::After,
		EditorToolbarButtonCommandList,
		FToolBarExtensionDelegate::CreateRaw(this, &FLevelSnapshotsEditorModule::CreateEditorToolbarButton));
	
	// Now insert the button into the main toolbar using the extension
	LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(NewToolbarExtender);
}

void FLevelSnapshotsEditorModule::MapEditorToolbarActions()
{
	EditorToolbarButtonCommandList = MakeShareable(new FUICommandList);

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().UseCreationFormToggle,
		FUIAction(
			FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::ToggleUseCreationForm),
			FCanExecuteAction(),
			FIsActionChecked::CreateRaw(this, &FLevelSnapshotsEditorModule::GetUseCreationForm)
		)
	);

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorToolbarButton,
		FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::OpenSnapshotsEditor)
	);

	EditorToolbarButtonCommandList->MapAction(
		FLevelSnapshotsEditorCommands::Get().LevelSnapshotsSettings,
		FExecuteAction::CreateStatic(&FLevelSnapshotsEditorModule::OpenLevelSnapshotsSettings)
	);
}

void FLevelSnapshotsEditorModule::CreateEditorToolbarButton(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateRaw(this, &FLevelSnapshotsEditorModule::BuildPathsToSaveSnapshotWithOptionalForm)),
		NAME_None,
		NSLOCTEXT("LevelSnapshots", "LevelSnapshots", "Level Snapshots"), // Set Text under image
		NSLOCTEXT("LevelSnapshots", "LevelSnapshotsToolbarButtonTooltip", "Take snapshot with optional form"), //  Set tooltip
		FSlateIcon(FLevelSnapshotsEditorStyle::GetStyleSetName(), "LevelSnapshots.ToolbarButton", "LevelSnapshots.ToolbarButton.Small") // Set image
	);
	
	Builder.AddComboButton(
		FUIAction(),
		FOnGetContent::CreateRaw(this, &FLevelSnapshotsEditorModule::FillEditorToolbarComboButtonMenuOptions, EditorToolbarButtonCommandList), // Add combo button subcommands menu
		FText::GetEmpty(),
		NSLOCTEXT("LevelSnapshots", "LevelSnapshotsToolbarComboButtonTooltip", "Open Level Snapshots Options") //  Set tooltip
	); 
}

TSharedRef<SWidget> FLevelSnapshotsEditorModule::FillEditorToolbarComboButtonMenuOptions(TSharedPtr<class FUICommandList> Commands)
{
	// Create FMenuBuilder instance for the commands we created
	FMenuBuilder MenuBuilder(true, Commands);

	// Then use it to add entries to the submenu of the combo button
	MenuBuilder.BeginSection("Creation", NSLOCTEXT("LevelSnapshots", "Creation", "Creation"));
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().UseCreationFormToggle);
	MenuBuilder.EndSection();
	MenuBuilder.AddMenuSeparator();
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().OpenLevelSnapshotsEditorToolbarButton);
	MenuBuilder.AddMenuEntry(FLevelSnapshotsEditorCommands::Get().LevelSnapshotsSettings);

	// Create the widget so it can be attached to the combo button
	return MenuBuilder.MakeWidget();
}

void FLevelSnapshotsEditorModule::BuildPathsToSaveSnapshotWithOptionalForm() const
{	
	check(ProjectSettingsObjectPtr.IsValid());
	check(DataMangementSettingsObjectPtr.IsValid());

	// Creation Form

	if (ProjectSettingsObjectPtr.Get()->bUseCreationForm)
	{
		TSharedRef<SWidget> CreationForm = 
			FLevelSnapshotsEditorCreationForm::MakeAndShowCreationWindow(
				FCloseCreationFormDelegate::CreateRaw(this, &FLevelSnapshotsEditorModule::HandleFormReply), 
				ProjectSettingsObjectPtr.Get(), DataMangementSettingsObjectPtr.Get());
	}
	else
	{
		TakeAndSaveSnapshot(FText::GetEmpty());
	}
}

void FLevelSnapshotsEditorModule::HandleFormReply(bool bWasCreateSnapshotPressed, FText InDescription) const
{
	if (bWasCreateSnapshotPressed)
	{
		TakeAndSaveSnapshot(InDescription, true);
	}
}

void FLevelSnapshotsEditorModule::TakeAndSaveSnapshot(const FText& InDescription, const bool bShouldUseOverrides) const
{
	if (!ensure(GEditor))
	{
		return;
	}
	UWorld* World = ULevelSnapshotsEditorData::GetEditorWorld();
	if (!ensure(World && ProjectSettingsObjectPtr.IsValid()))
	{
		return;
	}
	ULevelSnapshotsEditorDataManagementSettings* DataManagementSettings = DataMangementSettingsObjectPtr.Get();

	
	DataManagementSettings->ValidateRootLevelSnapshotSaveDirAsGameContentRelative();
	DataManagementSettings->SanitizeAllProjectSettingsPaths(true);

	const FText& NewSnapshotDir = ULevelSnapshotsEditorDataManagementSettings::ParseLevelSnapshotsTokensInText(
		FText::FromString(
			FPaths::Combine(DataManagementSettings->RootLevelSnapshotSaveDir.Path, DataManagementSettings->LevelSnapshotSaveDir)), World->GetName());
	const FText& NewSnapshotName = ULevelSnapshotsEditorDataManagementSettings::ParseLevelSnapshotsTokensInText(FText::FromString(
		bShouldUseOverrides && DataManagementSettings->IsNameOverridden() ?
		DataManagementSettings->GetNameOverride() : DataManagementSettings->DefaultLevelSnapshotName), World->GetName());

	const FString& ValidatedName = FPaths::MakeValidFileName(NewSnapshotName.ToString());

	ULevelSnapshotsEditorFunctionLibrary::TakeLevelSnapshotAndSaveToDisk(World, ValidatedName, NewSnapshotDir.ToString(), InDescription.ToString(), false);
}

void FLevelSnapshotsEditorModule::OpenLevelSnapshotsDialogWithAssetSelected(const FAssetData& InAssetData)
{
	OpenSnapshotsEditor();

	if (SnapshotEditorToolkit.IsValid())
	{
		SnapshotEditorToolkit.Pin()->OpenLevelSnapshotsDialogWithAssetSelected(InAssetData);
	}
}

void FLevelSnapshotsEditorModule::OpenSnapshotsEditor()
{
	if (SnapshotEditorToolkit.IsValid())
	{
		SnapshotEditorToolkit.Pin()->BringToolkitToFront();
	}
	else
	{
		ULevelSnapshotsEditorData* EditingObject = AllocateTransientPreset();
		SnapshotEditorToolkit = FLevelSnapshotsEditorToolkit::CreateSnapshotEditor(EditingObject);
	}
}

void FLevelSnapshotsEditorModule::OpenLevelSnapshotsSettings()
{
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "Level Snapshots");
}

ULevelSnapshotsEditorData* FLevelSnapshotsEditorModule::AllocateTransientPreset()
{
	static const TCHAR* PackageName = TEXT("/Temp/LevelSnapshots/PendingSnapshots");

	ULevelSnapshotsEditorData* ExistingPreset = FindObject<ULevelSnapshotsEditorData>(nullptr, TEXT("/Temp/LevelSnapshots/PendingSnapshots.PendingSnapshots"));
	if (ExistingPreset)
	{
		return ExistingPreset;
	}

	ULevelSnapshotsEditorData* TemplatePreset = GetDefault<ULevelSnapshotsUserSettings>()->LastEditorData.Get();

	static FName DesiredName = "PendingSnapshots";

	UPackage* NewPackage = CreatePackage(PackageName);
	NewPackage->SetFlags(RF_Transient);
	NewPackage->AddToRoot();

	ULevelSnapshotsEditorData* NewPreset = nullptr;

	if (TemplatePreset)
	{
		NewPreset = DuplicateObject<ULevelSnapshotsEditorData>(TemplatePreset, NewPackage, DesiredName);
		NewPreset->SetFlags(RF_Transient | RF_Transactional | RF_Standalone);
	}
	else
	{
		NewPreset = NewObject<ULevelSnapshotsEditorData>(NewPackage, DesiredName, RF_Transient | RF_Transactional | RF_Standalone);
	}

	return NewPreset;
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FLevelSnapshotsEditorModule, LevelSnapshotsEditor)
