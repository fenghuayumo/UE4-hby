// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FSequenceRecorderCommands : public TCommands<FSequenceRecorderCommands>
{
public:
	FSequenceRecorderCommands();

	TSharedPtr<FUICommandInfo> RecordAll;
	TSharedPtr<FUICommandInfo> StopAll;
	TSharedPtr<FUICommandInfo> AddRecording;
	TSharedPtr<FUICommandInfo> AddCurrentPlayerRecording;
	TSharedPtr<FUICommandInfo> RemoveRecording;
	TSharedPtr<FUICommandInfo> RemoveAllRecordings;
	TSharedPtr<FUICommandInfo> AddRecordingGroup;
	TSharedPtr<FUICommandInfo> RemoveRecordingGroup;
	TSharedPtr<FUICommandInfo> DuplicateRecordingGroup;

	/** Initialize commands */
	virtual void RegisterCommands() override;
};
