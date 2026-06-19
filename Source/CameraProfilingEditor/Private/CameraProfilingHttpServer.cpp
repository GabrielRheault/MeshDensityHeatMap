#include "CameraProfilingHttpServer.h"
#include "CameraProfilingTools.h"
#include "CameraProfilingSettings.h"

#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "HttpPath.h"
#include "HttpServerConstants.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraProfilingHttp, Log, All);

FCameraProfilingHttpServer& FCameraProfilingHttpServer::Get()
{
	static FCameraProfilingHttpServer Instance;
	return Instance;
}

namespace
{
	double QueryNumber(const FHttpServerRequest& Request, const FString& Key)
	{
		const FString* Value = Request.QueryParams.Find(Key);
		return Value ? FCString::Atod(**Value) : 0.0;
	}

	TUniquePtr<FHttpServerResponse> JsonResponse(const FString& Body)
	{
		// Build from a UTF-8 byte array (the most stable Create overload across engine versions).
		FTCHARToUTF8 Utf8(*Body);
		TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(MoveTemp(Bytes), TEXT("application/json"));
		// CORS so the file:// heat-map page is allowed to call us.
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		return Response;
	}
}

void FCameraProfilingHttpServer::Start()
{
	if (bStarted)
	{
		return;
	}
	const UCameraProfilingSettings* S = GetDefault<UCameraProfilingSettings>();
	BoundPort = S->GotoPort;

	FHttpServerModule& Http = FHttpServerModule::Get();
	Router = Http.GetHttpRouter(BoundPort);
	if (!Router.IsValid())
	{
		UE_LOG(LogCameraProfilingHttp, Warning, TEXT("[goto] could not bind 127.0.0.1:%d; bridge disabled."), BoundPort);
		return;
	}

	GotoCamRoute = Router->BindRoute(FHttpPath(TEXT("/gotocam")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FCameraProfilingHttpServer::HandleGotoCam));
	InspectRoute = Router->BindRoute(FHttpPath(TEXT("/inspectcell")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FCameraProfilingHttpServer::HandleInspect));

	Http.StartAllListeners();
	bStarted = true;
	UE_LOG(LogCameraProfilingHttp, Log, TEXT("[goto] bridge listening on 127.0.0.1:%d"), BoundPort);
}

void FCameraProfilingHttpServer::Stop()
{
	if (!bStarted)
	{
		return;
	}
	if (Router.IsValid())
	{
		if (GotoCamRoute.IsValid()) { Router->UnbindRoute(GotoCamRoute); }
		if (InspectRoute.IsValid()) { Router->UnbindRoute(InspectRoute); }
	}
	GotoCamRoute.Reset();
	InspectRoute.Reset();
	Router.Reset();
	bStarted = false;
}

bool FCameraProfilingHttpServer::HandleGotoCam(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const double X = QueryNumber(Request, TEXT("x"));
	const double Y = QueryNumber(Request, TEXT("y"));
	const double Z = QueryNumber(Request, TEXT("z"));
	const double Pitch = QueryNumber(Request, TEXT("pitch"));
	const double Yaw = QueryNumber(Request, TEXT("yaw"));
	const double Roll = QueryNumber(Request, TEXT("roll"));
	FCameraProfilingTools::GotoCamera(X, Y, Z, Pitch, Yaw, Roll);
	OnComplete(JsonResponse(TEXT("{\"ok\":true}")));
	return true;
}

bool FCameraProfilingHttpServer::HandleInspect(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const double X = QueryNumber(Request, TEXT("x"));
	const double Y = QueryNumber(Request, TEXT("y"));
	const double Size = QueryNumber(Request, TEXT("size"));
	const FString Breakdown = FCameraProfilingTools::InspectCell(X, Y, Size);
	OnComplete(JsonResponse(Breakdown));
	return true;
}
