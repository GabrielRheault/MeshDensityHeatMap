#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FCameraProfilingEditorModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	/** Registered with UToolMenus::RegisterStartupCallback; builds the Tools -> Yes Chef menu. */
	FDelegateHandle MenuStartupHandle;
};
