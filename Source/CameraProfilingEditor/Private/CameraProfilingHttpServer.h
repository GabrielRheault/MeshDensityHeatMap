#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

/**
 * Localhost HTTP bridge so the (file://) heat-map page can drive the editor:
 *   GET /goto?x=&y=            -> move the active level-editor viewport to the cell
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
	bool HandleGoto(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleInspect(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle GotoRoute;
	FHttpRouteHandle InspectRoute;
	int32 BoundPort = 0;
	bool bStarted = false;
};
