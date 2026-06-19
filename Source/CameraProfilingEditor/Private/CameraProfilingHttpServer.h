#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

/**
 * Localhost HTTP bridge so the (file://) heat-map page can drive the editor:
 *   GET /gotocam?x=&y=&z=&pitch=&yaw=&roll= -> move the viewport to that camera's exact transform
 *   GET /inspectcell?x=&y=&size= -> select the cell's meshes/instances + return a JSON breakdown
 *
 * Handlers run on the game thread (the HttpServer module ticks there), so they call GEditor directly.
 */
class FCameraProfilingHttpServer
{
public:
	static FCameraProfilingHttpServer& Get();

	void Start();
	void Stop();

private:
	bool HandleGotoCam(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleInspect(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle GotoCamRoute;
	FHttpRouteHandle InspectRoute;
	int32 BoundPort = 0;
	bool bStarted = false;
};
