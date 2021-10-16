// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;

public class OpenGL : ModuleRules
{
	public OpenGL(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		PublicIncludePaths.Add(ModuleDirectory);

		if ((Target.Platform == UnrealTargetPlatform.Win64) ||
			(Target.Platform == UnrealTargetPlatform.Win32))
		{
			PublicSystemLibraries.Add("opengl32.lib");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.Add("OpenGL");
			PublicFrameworks.Add("QuartzCore");
		}
		else if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			PublicFrameworks.Add("OpenGLES");
		}
	}
}
