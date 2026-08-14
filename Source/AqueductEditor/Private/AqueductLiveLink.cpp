#include "AqueductLiveLink.h"
#include "AqueductMaterialManager.h"
#include "AqueductPaths.h"
#include "IWebSocketNetworkingModule.h"
#include "IWebSocketServer.h"
#include "INetworkingWebSocket.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/RunnableThread.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"

DEFINE_LOG_CATEGORY_STATIC(LogAqueductLiveLink, Log, All);

// Machine-matched ack error: the frontend (frontend/src/lib/liveLink.ts,
// SOURCE_MISMATCH) compares this exact string to raise the replace dialog.
static const TCHAR* const SourceMismatchError = TEXT("source_mismatch");



FAqueductLiveLink& FAqueductLiveLink::Get()
{
	static FAqueductLiveLink Instance;
	return Instance;
}

// ---------------------------------------------------------------------------
// Service thread: exclusive owner of the lws server and all sockets.
// ---------------------------------------------------------------------------

EWebsocketConnectionFilterResult FAqueductLiveLink::FServiceThread::FilterConnection(
	FString Origin, FString ClientIP)
{
	// Browsers always send Origin and pages cannot forge it — this closes the
	// any-website-connects-to-localhost hole. Non-browser clients (no Origin)
	// are local processes, which is already an execution context we trust.
	if (Origin.IsEmpty())
	{
		return EWebsocketConnectionFilterResult::ConnectionAccepted;
	}

	const FString Lower = Origin.ToLower().TrimStartAndEnd();
	const bool bAllowed =
		Lower == TEXT("https://aqueduct.sh") ||
		Lower == TEXT("https://www.aqueduct.sh") ||
		Lower == TEXT("http://localhost") ||
		Lower == TEXT("http://127.0.0.1") ||
		Lower.StartsWith(TEXT("http://localhost:")) ||
		Lower.StartsWith(TEXT("http://127.0.0.1:"));

	if (!bAllowed)
	{
		UE_LOG(LogAqueductLiveLink, Warning,
			TEXT("Live link refused connection from origin '%s' (%s)"), *Origin, *ClientIP);
		return EWebsocketConnectionFilterResult::ConnectionRefused;
	}
	return EWebsocketConnectionFilterResult::ConnectionAccepted;
}

void FAqueductLiveLink::FServiceThread::OnClientConnected(INetworkingWebSocket* Socket)
{
	if (!Socket) return;

	const uint64 SocketId = NextSocketId++;
	Sockets.Add(SocketId, Socket);
	SocketIds.Add(Socket, SocketId);

	Socket->SetReceiveCallBack(FWebSocketPacketReceivedCallBack::CreateLambda(
		[this, SocketId](void* Data, int32 Size)
		{
			FInboundEvent Event;
			Event.Type = FInboundEvent::EType::Message;
			Event.SocketId = SocketId;
			Event.Payload.Append(static_cast<const uint8*>(Data), Size);
			Inbound.Enqueue(MoveTemp(Event));
		}));
	Socket->SetSocketClosedCallBack(FWebSocketInfoCallBack::CreateLambda(
		[this, Socket]()
		{
			uint64 ClosedId = 0;
			if (SocketIds.RemoveAndCopyValue(Socket, ClosedId))
			{
				Sockets.Remove(ClosedId);
				FInboundEvent Event;
				Event.Type = FInboundEvent::EType::Closed;
				Event.SocketId = ClosedId;
				Inbound.Enqueue(MoveTemp(Event));
			}
		}));

	// Greeting sent from THIS thread: the client learns the connection is
	// live immediately, never waiting on an editor frame.
	const FString Hello = FString::Printf(
		TEXT("{\"type\":\"hello\",\"port\":%d,\"version\":\"%s\"}"),
		BoundPort.load(), *PluginVersion);
	FTCHARToUTF8 Utf8(*Hello);
	Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), /*bPrependSize=*/false);

	FInboundEvent Event;
	Event.Type = FInboundEvent::EType::Connected;
	Event.SocketId = SocketId;
	Inbound.Enqueue(MoveTemp(Event));
}

int32 FAqueductLiveLink::FServiceThread::ResolveListenPort()
{
	return DefaultPort;
}

uint32 FAqueductLiveLink::FServiceThread::Run()
{
	const int32 ListenPort = ResolveListenPort();

	IWebSocketNetworkingModule& Module =
		FModuleManager::LoadModuleChecked<IWebSocketNetworkingModule>(TEXT("WebSocketNetworking"));

	TUniquePtr<IWebSocketServer> Candidate = Module.CreateServer();
	if (Candidate.IsValid())
	{
		Candidate->SetFilterConnectionCallback(
			FWebSocketFilterConnectionCallback::CreateStatic(&FServiceThread::FilterConnection));

		if (Candidate->Init(
				ListenPort,
				FWebSocketClientConnectedCallBack::CreateRaw(
					this, &FServiceThread::OnClientConnected),
				TEXT("127.0.0.1")))
		{
			Server = MoveTemp(Candidate);
			BoundPort.store(ListenPort);
		}
	}

	if (!Server.IsValid())
	{
		UE_LOG(LogAqueductLiveLink, Error,
			TEXT("Live link could not bind 127.0.0.1:%d. Another process is ")
			TEXT("occupying it (a second editor instance?)."), ListenPort);
		BoundPort.store(0);
		return 1;
	}

	UE_LOG(LogAqueductLiveLink, Log, TEXT("Live link listening on 127.0.0.1:%d"),
		BoundPort.load());

	// Service loop: ~2ms cadence. lws_service(ctx, 0) is a non-blocking
	// poll; an idle iteration is a few microseconds. Handshake latency is
	// therefore ~milliseconds regardless of what the editor is doing.
	while (!bStopRequested.load())
	{
		FOutboundSend Send;
		while (Outbound.Dequeue(Send))
		{
			if (INetworkingWebSocket** Socket = Sockets.Find(Send.SocketId))
			{
				(*Socket)->Send(Send.Payload.GetData(), Send.Payload.Num(), /*bPrependSize=*/false);
			}
		}

		Server->Tick();
		FPlatformProcess::Sleep(0.002f);
	}

	// Teardown on the owning thread.
	Sockets.Empty();
	SocketIds.Empty();
	Server.Reset();
	return 0;
}

// ---------------------------------------------------------------------------
// Lifecycle (game thread)
// ---------------------------------------------------------------------------

void FAqueductLiveLink::Start()
{
	if (Service.IsValid()) return;

	// The module must be loaded from the game thread before the service
	// thread uses it (module manager loads are not thread-safe).
	FModuleManager::LoadModuleChecked<IWebSocketNetworkingModule>(TEXT("WebSocketNetworking"));

	Service = MakeUnique<FServiceThread>();
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Aqueduct")))
	{
		Service->PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	ServiceRunner.Reset(FRunnableThread::Create(
		Service.Get(), TEXT("AqueductLiveLink"), 0, TPri_Normal));

	bListeningLogged = false;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FAqueductLiveLink::Tick), 0.0f);
}

void FAqueductLiveLink::Stop()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// Apply any staged edits so the last state isn't lost. No bakes here:
	// edits already marked their packages dirty, and the pre-save hook
	// bakes when (and only when) a save needs the pixels.
	FlushPendingSyncs();
	Clients.Empty();

	if (ServiceRunner.IsValid())
	{
		ServiceRunner->Kill(/*bShouldWait=*/true); // signals Stop(), joins
		ServiceRunner.Reset();
	}
	Service.Reset();
	BoundPort = 0;
}

// ---------------------------------------------------------------------------
// Game-thread tick: drain inbound events, coalesce syncs, deferred saveall.
// ---------------------------------------------------------------------------

bool FAqueductLiveLink::Tick(float DeltaTime)
{
	if (Service.IsValid())
	{
		if (!bListeningLogged)
		{
			const int32 Port = Service->BoundPort.load();
			if (Port > 0)
			{
				BoundPort = Port;
				bListeningLogged = true;
			}
			else if (Port == 0)
			{
				bListeningLogged = true; // bind failed; error already logged
			}
		}

		FInboundEvent Event;
		while (Service->Inbound.Dequeue(Event))
		{
			HandleInboundEvent(Event);
		}
	}

	// Apply only the latest staged payload per graph, once per frame.
	FlushPendingSyncs();
	return true;
}

void FAqueductLiveLink::HandleInboundEvent(FInboundEvent& Event)
{
	switch (Event.Type)
	{
	case FInboundEvent::EType::Connected:
	{
		// Connections are intent-driven (the editor-side toggle), so the
		// background-CPU throttle lifts at connect: during live editing the
		// user's focus is in the browser, and a throttled editor would cap
		// updates at a few fps. The setting is read live every frame by the
		// editor (no notification needed) and is restored, not persisted.
		if (Clients.Num() == 0)
		{
			UEditorPerformanceSettings* Perf = GetMutableDefault<UEditorPerformanceSettings>();
			bSavedThrottleSetting = Perf->bThrottleCPUWhenNotForeground;
			Perf->bThrottleCPUWhenNotForeground = false;
		}
		Clients.Add(Event.SocketId, FClientState());
		UE_LOG(LogAqueductLiveLink, Log, TEXT("Live link client connected (#%llu)"),
			Event.SocketId);
		break;
	}

	case FInboundEvent::EType::Message:
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Event.Payload.GetData()),
			Event.Payload.Num());
		OnMessage(Event.SocketId, FString(Converter.Length(), Converter.Get()));
		break;
	}

	case FInboundEvent::EType::Closed:
	{
		FClientState State;
		if (Clients.RemoveAndCopyValue(Event.SocketId, State))
		{
			// No bake on disconnect: the session's edits marked their
			// packages dirty; pixels are baked lazily by the pre-save hook
			// when the user saves (or by the hash guard at next startup).
			UE_LOG(LogAqueductLiveLink, Log,
				TEXT("Live link client disconnected (%d touched graph(s), packages dirty)"),
				State.TouchedGraphs.Num());

			if (Clients.Num() == 0)
			{
				GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground =
					bSavedThrottleSetting;
			}
		}
		break;
	}
	}
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void FAqueductLiveLink::OnMessage(uint64 SocketId, const FString& Message)
{
	TSharedPtr<FJsonObject> Msg;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Msg) || !Msg.IsValid())
	{
		SendAck(SocketId, -1, FString(), false, TEXT("malformed JSON"));
		return;
	}

	const FString Type = Msg->GetStringField(TEXT("type"));
	if (Type == TEXT("sync") || Type == TEXT("params"))
	{
		HandleSync(SocketId, Msg);
	}
	else if (Type == TEXT("bake"))
	{
		HandleBake(SocketId, Msg);
	}
	else if (Type == TEXT("stats"))
	{
		HandleStats(SocketId, Msg);
	}
	else if (Type == TEXT("ping"))
	{
		TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(TEXT("type"), TEXT("pong"));
		SendJson(SocketId, Pong);
	}
	else
	{
		SendAck(SocketId,
			Msg->HasTypedField<EJson::Number>(TEXT("seq")) ? Msg->GetNumberField(TEXT("seq")) : -1,
			FString(), false, FString::Printf(TEXT("unknown message type '%s'"), *Type));
	}
}

FString FAqueductLiveLink::ResolveGraphPath(
	const FString& GraphName, const FString& SourceDocumentId)
{
	if (!SourceDocumentId.IsEmpty())
	{
		const FString EstablishedPath =
			FAqueductMaterialManager::Get().ResolveGraphPathBySourceDocumentId(
				SourceDocumentId);
		if (!EstablishedPath.IsEmpty()) return EstablishedPath;
	}

	// Name validity is a core convention (shared with the web editor's
	// sanitizer and the CLI), not a per-consumer regex.
	if (!AqueductConventions::IsValidGraphName(GraphName)) return FString();
	// Placement is the material manager's single convention: existing
	// recipes keep their location, new graphs get per-graph subfolders.
	return FAqueductMaterialManager::Get().ResolveGraphPath(GraphName);
}

void FAqueductLiveLink::HandleSync(uint64 SocketId, const TSharedPtr<FJsonObject>& Msg)
{
	const bool bParamFrame = Msg->GetStringField(TEXT("type")) == TEXT("params");
	const double Seq = Msg->HasTypedField<EJson::Number>(TEXT("seq")) ? Msg->GetNumberField(TEXT("seq")) : -1;
	const FString GraphName = Msg->GetStringField(TEXT("graph"));
	const FString SourceId = Msg->HasTypedField<EJson::String>(TEXT("source"))
		? Msg->GetStringField(TEXT("source")) : FString();
	FString Toml;
	if (!Msg->TryGetStringField(TEXT("toml"), Toml) || Toml.IsEmpty())
	{
		SendAck(SocketId, Seq, GraphName, false, TEXT("missing toml payload"));
		return;
	}

	const FString GraphPath = ResolveGraphPath(GraphName, SourceId);
	if (GraphPath.IsEmpty())
	{
		SendAck(SocketId, Seq, GraphName, false, TEXT("invalid graph name"));
		return;
	}

	TArray<FPendingSync::FParamEdit> IncomingEdits;
	if (bParamFrame)
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonEdits = nullptr;
		if (!Msg->TryGetArrayField(TEXT("edits"), JsonEdits) || !JsonEdits
			|| JsonEdits->Num() == 0)
		{
			SendAck(SocketId, Seq, GraphName, false, TEXT("missing numeric param edits"));
			return;
		}
		for (const TSharedPtr<FJsonValue>& JsonEdit : *JsonEdits)
		{
			const TSharedPtr<FJsonObject> EditObject = JsonEdit ? JsonEdit->AsObject() : nullptr;
			FPendingSync::FParamEdit Edit;
			if (!EditObject || !EditObject->TryGetStringField(TEXT("node"), Edit.NodeId)
				|| !EditObject->TryGetStringField(TEXT("param"), Edit.ParamName)
				|| !EditObject->TryGetNumberField(TEXT("value"), Edit.Value)
				|| Edit.NodeId.IsEmpty() || Edit.ParamName.IsEmpty())
			{
				SendAck(SocketId, Seq, GraphName, false, TEXT("invalid numeric param edit"));
				return;
			}
			Edit.bInteger = EditObject->HasTypedField<EJson::Boolean>(TEXT("integer"))
				&& EditObject->GetBoolField(TEXT("integer"));
			IncomingEdits.Add(MoveTemp(Edit));
		}
	}

	// Stage, latest-wins: a superseded edit's pixels can never be observed
	// (its successor is already here), so it is acked as coalesced
	// immediately and only the newest payload is processed this frame.
	bool bRequiresFullReload = !bParamFrame;
	TArray<FPendingSync::FParamEdit> MergedEdits;
	if (FPendingSync* Existing = PendingSyncs.Find(GraphPath))
	{
		bRequiresFullReload |= Existing->bRequiresFullReload;
		if (!bRequiresFullReload)
		{
			MergedEdits = Existing->ParamEdits;
		}
		SendAck(Existing->SocketId, Existing->Seq, Existing->GraphName, true,
			FString(), /*ReloadMs=*/0.0);
	}
	if (!bRequiresFullReload)
	{
		for (FPendingSync::FParamEdit& Incoming : IncomingEdits)
		{
			const int32 ExistingIndex = MergedEdits.IndexOfByPredicate(
				[&Incoming](const FPendingSync::FParamEdit& Candidate)
				{
					return Candidate.NodeId == Incoming.NodeId
						&& Candidate.ParamName == Incoming.ParamName;
				});
			if (ExistingIndex == INDEX_NONE) MergedEdits.Add(MoveTemp(Incoming));
			else MergedEdits[ExistingIndex] = MoveTemp(Incoming);
		}
	}
	FPendingSync& Pending = PendingSyncs.FindOrAdd(GraphPath);
	Pending.GraphPath = GraphPath;
	Pending.GraphName = GraphName;
	Pending.Toml = MoveTemp(Toml);
	Pending.SourceId = SourceId;
	Pending.bForce = Msg->HasTypedField<EJson::Boolean>(TEXT("force"))
		&& Msg->GetBoolField(TEXT("force"));
	Pending.bRequiresFullReload = bRequiresFullReload;
	Pending.ParamEdits = MoveTemp(MergedEdits);
	Pending.Seq = Seq;
	Pending.SocketId = SocketId;
}

void FAqueductLiveLink::ProcessPendingSync(FPendingSync& Pending)
{
	const double StartTime = FPlatformTime::Seconds();

	// The socket may have closed between staging and processing.
	const uint64 SocketId = Clients.Contains(Pending.SocketId) ? Pending.SocketId : 0;

	// Version control: workspace files may be read-only until checked out.
	FString WriteError;
	if (!FAqueductMaterialManager::EnsureFileWritable(Pending.GraphPath, WriteError))
	{
		if (SocketId) SendAck(SocketId, Pending.Seq, Pending.GraphName, false, WriteError);
		return;
	}

	auto& Manager = FAqueductMaterialManager::Get();
	Manager.EnsureInitialized();

	// Document-identity guard: a sync from a DIFFERENT cloud document than
	// the one this graph is stamped with is refused (error
	// "source_mismatch") so the editor can ask before overwriting. The
	// user's "Replace it" resends with force.
	if (!Manager.AcquireSourceOwnership(Pending.GraphPath, Pending.SourceId, Pending.bForce))
	{
		if (SocketId)
		{
			SendAck(SocketId, Pending.Seq, Pending.GraphName, false, SourceMismatchError);
		}
		return;
	}

	// The file on disk is the single source of truth: write it, suppress the
	// watcher echo, and ingest through the same reload path external edits use.
	const bool bNewRecipe = !FPaths::FileExists(Pending.GraphPath);
	if (bNewRecipe)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Pending.GraphPath), /*Tree=*/true);
	}
	Manager.SuppressNextWatcherEvent(Pending.GraphPath);
	if (!FFileHelper::SaveStringToFile(Pending.Toml, *Pending.GraphPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		if (SocketId)
		{
			SendAck(SocketId, Pending.Seq, Pending.GraphName, false,
				FString::Printf(TEXT("failed to write %s"), *Pending.GraphPath));
		}
		return;
	}
	if (bNewRecipe)
	{
		// The recipe is source code (design doc): a brand-new graph file must
		// travel with the assets the editor's save flow will add later.
		FAqueductMaterialManager::MarkFileForAdd(Pending.GraphPath);
	}

	bool bReloaded = false;
	if (!Pending.bRequiresFullReload && Pending.ParamEdits.Num() > 0)
	{
		TArray<FAqueductMaterialManager::FNumericParamEdit> Edits;
		Edits.Reserve(Pending.ParamEdits.Num());
		for (const FPendingSync::FParamEdit& PendingEdit : Pending.ParamEdits)
		{
			FAqueductMaterialManager::FNumericParamEdit& Edit = Edits.AddDefaulted_GetRef();
			Edit.NodeId = PendingEdit.NodeId;
			Edit.ParamName = PendingEdit.ParamName;
			Edit.Value = PendingEdit.Value;
			Edit.bInteger = PendingEdit.bInteger;
		}
		bReloaded = Manager.ApplyLiveParameterEdits(Pending.GraphPath, Pending.Toml, Edits);
	}
	if (!bReloaded)
	{
		bReloaded = Manager.ReloadGraphFromFile(Pending.GraphPath);
	}
	const double ReloadMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (bReloaded && SocketId)
	{
		if (FClientState* State = Clients.Find(SocketId))
		{
			State->TouchedGraphs.Add(Pending.GraphPath);
		}
	}

	if (SocketId)
	{
		SendAck(SocketId, Pending.Seq, Pending.GraphName, bReloaded,
			bReloaded ? FString() : TEXT("graph reload failed (see UE log)"), ReloadMs);
	}
}

int32 FAqueductLiveLink::FlushPendingSyncs()
{
	if (PendingSyncs.Num() == 0) return 0;
	TMap<FString, FPendingSync> ToProcess = MoveTemp(PendingSyncs);
	PendingSyncs.Reset();
	for (auto& Pair : ToProcess)
	{
		ProcessPendingSync(Pair.Value);
	}
	return ToProcess.Num();
}

void FAqueductLiveLink::HandleBake(uint64 SocketId, const TSharedPtr<FJsonObject>& Msg)
{
	// Wire order: any edit staged before this verb must be applied first.
	FlushPendingSyncs();

	const double Seq = Msg->HasTypedField<EJson::Number>(TEXT("seq")) ? Msg->GetNumberField(TEXT("seq")) : -1;
	const FString GraphName = Msg->GetStringField(TEXT("graph"));
	const FString GraphPath = ResolveGraphPath(GraphName);
	if (GraphPath.IsEmpty())
	{
		SendAck(SocketId, Seq, GraphName, false, TEXT("invalid graph name"));
		return;
	}
	const bool bOk = FAqueductMaterialManager::Get().BakeGraph(GraphPath);
	SendAck(SocketId, Seq, GraphName, bOk, bOk ? FString() : TEXT("bake failed (see UE log)"));
}


void FAqueductLiveLink::HandleStats(uint64 SocketId, const TSharedPtr<FJsonObject>& Msg)
{
	// Wire order: any edit staged before this request must be applied first
	// so the stats reflect the latest synced graph.
	FlushPendingSyncs();

	const double Seq = Msg->HasTypedField<EJson::Number>(TEXT("seq")) ? Msg->GetNumberField(TEXT("seq")) : -1;
	const FString GraphName = Msg->GetStringField(TEXT("graph"));
	const FString SourceId = Msg->HasTypedField<EJson::String>(TEXT("source"))
		? Msg->GetStringField(TEXT("source")) : FString();

	const FString GraphPath = ResolveGraphPath(GraphName, SourceId);
	if (GraphPath.IsEmpty())
	{
		SendAck(SocketId, Seq, GraphName, false, TEXT("invalid graph name"));
		return;
	}

	const FString VramJson = FAqueductMaterialManager::Get().GetVramStatsJson(GraphPath);

	TSharedRef<FJsonObject> Reply = MakeShared<FJsonObject>();
	Reply->SetStringField(TEXT("type"), TEXT("stats"));
	Reply->SetNumberField(TEXT("seq"), Seq);
	Reply->SetStringField(TEXT("graph"), GraphName);
	if (!VramJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> Vram;
		const TSharedRef<TJsonReader<>> VramReader = TJsonReaderFactory<>::Create(VramJson);
		if (FJsonSerializer::Deserialize(VramReader, Vram) && Vram.IsValid())
		{
			Reply->SetObjectField(TEXT("vram"), Vram);
		}
	}
	SendJson(SocketId, Reply);
}


// ---------------------------------------------------------------------------
// Send helpers (game thread → outbound queue → service thread)
// ---------------------------------------------------------------------------

void FAqueductLiveLink::SendJson(uint64 SocketId, const TSharedRef<FJsonObject>& Obj)
{
	if (!Service.IsValid() || SocketId == 0) return;

	FString Payload;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Payload);
	FJsonSerializer::Serialize(Obj, Writer);

	FTCHARToUTF8 Utf8(*Payload);
	FOutboundSend Send;
	Send.SocketId = SocketId;
	Send.Payload.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Service->Outbound.Enqueue(MoveTemp(Send));
}

void FAqueductLiveLink::SendAck(uint64 SocketId, double Seq, const FString& Graph,
	bool bOk, const FString& Error, double ReloadMs)
{
	TSharedRef<FJsonObject> Ack = MakeShared<FJsonObject>();
	Ack->SetStringField(TEXT("type"), TEXT("ack"));
	Ack->SetNumberField(TEXT("seq"), Seq);
	Ack->SetStringField(TEXT("graph"), Graph);
	Ack->SetBoolField(TEXT("ok"), bOk);
	if (!Error.IsEmpty()) Ack->SetStringField(TEXT("error"), Error);
	if (ReloadMs > 0.0) Ack->SetNumberField(TEXT("reloadMs"), ReloadMs);
	SendJson(SocketId, Ack);
}
