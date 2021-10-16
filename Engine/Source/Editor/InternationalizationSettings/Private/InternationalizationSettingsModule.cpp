// Copyright Epic Games, Inc. All Rights Reserved.

#include "InternationalizationSettingsModule.h"

#include "InternationalizationSettingsModelDetails.h"
#include "PropertyEditorModule.h"


class FInternationalizationSettingsModule : public IInternationalizationSettingsModule
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

IMPLEMENT_MODULE(FInternationalizationSettingsModule, InternationalizationSettings)

void FInternationalizationSettingsModule::StartupModule()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout("InternationalizationSettingsModel", FOnGetDetailCustomizationInstance::CreateStatic(&FInternationalizationSettingsModelDetails::MakeInstance));
}

void FInternationalizationSettingsModule::ShutdownModule()
{
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomPropertyTypeLayout("InternationalizationSettingsModel");
	}
}