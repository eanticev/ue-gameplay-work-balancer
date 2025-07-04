

using UnrealBuildTool;

public class GWBEditor : ModuleRules
{
	public GWBEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// Enable C++20 for std::source_location support
		CppStandard = CppStandardVersion.Cpp20;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
			
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"GWBRuntime",
				"GWBCustomNodesRuntime",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"BlueprintGraph",
				"KismetCompiler",
				"ToolMenus",
				"EditorStyle",
				"EditorWidgets",
				"GraphEditor",
				"Kismet",
				"PropertyEditor",
				"SlateCore",
				"Slate",
				"ToolMenus",
				"EditorSubsystem"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}