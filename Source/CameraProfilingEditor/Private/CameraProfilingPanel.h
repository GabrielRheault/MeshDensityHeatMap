#pragma once

#include "CoreMinimal.h"

/** Dockable "Camera Profiling" tab: a details view of the settings (grid, bounds, placement, …)
 *  plus the Generate / Profile / See-Map / Refresh action buttons. */
namespace CameraProfilingPanel
{
	void Register();    // register the nomad tab spawner (call from module startup)
	void Unregister();  // call from module shutdown
	void Open();        // invoke/focus the tab (menu action)
}
