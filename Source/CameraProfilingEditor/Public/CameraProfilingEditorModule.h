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
};
