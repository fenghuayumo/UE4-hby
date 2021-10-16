// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Empty base class for handlers that are maintained for the lifetime of the compiler context */
class ANIMGRAPH_API IAnimBlueprintCompilerHandler
{
public:
	virtual ~IAnimBlueprintCompilerHandler() {}
};
