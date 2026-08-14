#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/Runnable.h"
#include "WebSocketNetworkingDelegates.h"
#include <atomic>

class IWebSocketServer;
class INetworkingWebSocket;
class FRunnableThread;

/**
 * Live-link WebSocket server: the low-latency channel between the Aqueduct
 * web editor (or local app) and this plugin.
 *
 * THREADING. The engine's WebSocketNetworking server is a libwebsockets
 * event loop serviced ONLY by explicit Tick() calls — and a tick-driven
 * server on the game thread makes connection establishment hostage to
 * editor frame timing: an unfocused editor is background-throttled to a
 * few fps, and any modal dialog or load stops ticking entirely, so the
 * TCP connection is accepted by the kernel but the WebSocket handshake
 * response never arrives before client timeouts ("works intermittently").
 * The server therefore runs on a DEDICATED SERVICE THREAD that owns it
 * exclusively: creation, lws servicing (~2ms cadence), every socket Send,
 * and destruction all happen there. Handshakes and acks are independent
 * of editor frame rate. The game thread exchanges data through two
 * single-producer queues: inbound events (connect/message/close) drained
 * by the ticker, outbound payloads drained by the service loop. Graph
 * reloads still run on the game thread (inherently — they touch UObjects
 * and the GPU pipeline), so pixel updates wait on editor frames, but the
 * LINK never does.
 *
 * Data flows ONE way (editor → plugin). Every edit carries the entire
 * serialized graph TOML, which the plugin writes to
 * Content/Aqueduct/<graph>/<graph>.aqueduct. Numeric slider frames also
 * identify their changed uniforms so an already-compiled graph can replay
 * its dirty dependency cone without parsing the whole document. Structural
 * edits and any unsafe fast-path request use the normal reload-from-file
 * handler. The file on disk remains the single source of truth.
 *
 * - Binds 127.0.0.1 only (no LAN exposure, no firewall prompts).
 * - Binds one fixed loopback port (FServiceThread::DefaultPort); if another
 *   process holds it (a second editor instance), the live link stays off
 *   for this session.
 * - Browser Origin allowlist (the Aqueduct site + localhost origins)
 *   closes the any-website-connects-to-localhost hole. Non-browser clients
 *   (no Origin header) are local processes and are allowed.
 *
 * Protocol (JSON text frames):
 *   → {"type":"sync","graph":"<name>","toml":"...","seq":N,
 *      "source":"<doc id>"?, "force":true?}
 *   → {"type":"params","graph":"<name>","toml":"...","seq":N,
 *      "edits":[{"node":"id","param":"name","value":1.2,"integer":false}]}
 *     source: cloud document id (absent for local files). A sync whose id
 *     differs from the target graph's stamp is refused with
 *     error "source_mismatch"; force overwrites and re-stamps.
 *   ← {"type":"ack","seq":N,"graph":"...","ok":true|false,
 *      "error":"...","reloadMs":1.2}
 *   → {"type":"bake","graph":"<name>"}            (explicit bake request)
 *   → {"type":"ping"} ← {"type":"pong"}
 *   ← {"type":"hello","port":N} greeting on connect (sent by the service
 *     thread itself, so it never waits on an editor frame)
 */
class FAqueductLiveLink
{
public:
	static FAqueductLiveLink& Get();

	/** Start the service thread and listen (idempotent). */
	void Start();

	/** Stop the server and join the service thread. */
	void Stop();

	int32 GetBoundPort() const { return BoundPort; }

private:
	FAqueductLiveLink() = default;

	// -- Cross-thread plumbing ---------------------------------------------

	struct FInboundEvent
	{
		enum class EType : uint8 { Connected, Message, Closed };
		EType Type = EType::Message;
		uint64 SocketId = 0;
		TArray<uint8> Payload; // Message only
	};

	struct FOutboundSend
	{
		uint64 SocketId = 0;
		TArray<uint8> Payload;
	};

	/**
	 * The service thread: exclusive owner of the WebSocket server and all
	 * socket objects. Game-thread code never touches them — sockets cross
	 * the boundary as opaque ids only.
	 */
	class FServiceThread : public FRunnable
	{
	public:
		/** The live-link port the frontend connects to. One fixed port, no
		 *  fallback: the frontend and the plugin must always agree on where
		 *  to meet. Not a user setting. */
		static constexpr int32 DefaultPort = 19014;

		/** The port to bind, resolved at thread start. */
		static int32 ResolveListenPort();

		// FRunnable
		virtual uint32 Run() override;
		virtual void Stop() override { bStopRequested.store(true); }

		/** -1 while binding, 0 on failure, >0 the bound port. */
		std::atomic<int32> BoundPort{-1};

		/** From the .uplugin descriptor; set on the game thread before the
		 *  thread starts, reported to the web editor in the hello. */
		FString PluginVersion;

		/** Service thread → game thread (drained by the ticker). */
		TQueue<FInboundEvent, EQueueMode::Spsc> Inbound;

		/** Game thread → service thread (drained by the service loop). */
		TQueue<FOutboundSend, EQueueMode::Spsc> Outbound;

	private:
		static EWebsocketConnectionFilterResult FilterConnection(FString Origin, FString ClientIP);
		void OnClientConnected(INetworkingWebSocket* Socket);

		std::atomic<bool> bStopRequested{false};

		TUniquePtr<IWebSocketServer> Server;
		uint64 NextSocketId = 1;
		TMap<uint64, INetworkingWebSocket*> Sockets;
		TMap<INetworkingWebSocket*, uint64> SocketIds;
	};

	// -- Game-thread side ----------------------------------------------------

	bool Tick(float DeltaTime);
	void HandleInboundEvent(FInboundEvent& Event);
	void OnMessage(uint64 SocketId, const FString& Message);

	void HandleSync(uint64 SocketId, const TSharedPtr<class FJsonObject>& Msg);
	void HandleBake(uint64 SocketId, const TSharedPtr<class FJsonObject>& Msg);


	/** Product verb: VRAM bookkeeping for a graph's executor working set.
	 *  Replies {"type":"stats","graph":...,"vram":{...}} in the camelCase
	 *  contract shared with the web frontend. */
	void HandleStats(uint64 SocketId, const TSharedPtr<class FJsonObject>& Msg);

	void SendJson(uint64 SocketId, const TSharedRef<class FJsonObject>& Obj);
	void SendAck(uint64 SocketId, double Seq, const FString& Graph,
		bool bOk, const FString& Error = FString(), double ReloadMs = 0.0);

	/** Resolve an established cloud source first, otherwise validate the
	 *  proposed graph name and use the normal first-sync placement. */
	static FString ResolveGraphPath(
		const FString& GraphName, const FString& SourceDocumentId = FString());

	TUniquePtr<FServiceThread> Service;
	TUniquePtr<FRunnableThread> ServiceRunner;
	int32 BoundPort = 0;
	bool bListeningLogged = false;
	FTSTicker::FDelegateHandle TickerHandle;

	/**
	 * Server-side edit coalescing. The browser coalesces its sends to one
	 * per animation frame, but messages still accumulate IN FLIGHT whenever
	 * the editor's frame runs longer than the send interval. Sync payloads
	 * are STAGED here (latest TOML per graph wins; superseded seqs are
	 * acked as coalesced immediately) and processed at most once per graph
	 * per tick: a burst of N edits costs one file write and one reload.
	 */
	struct FPendingSync
	{
		struct FParamEdit
		{
			FString NodeId;
			FString ParamName;
			double Value = 0.0;
			bool bInteger = false;
		};

		FString GraphPath;
		FString GraphName;
		FString Toml;
		/** Cloud document id of the syncing editor document (empty for
		 *  local-file documents). Checked against the wrapper's stamp; a
		 *  mismatch refuses the sync with error "source_mismatch" so the
		 *  editor can ask before a different document overwrites the graph. */
		FString SourceId;
		/** User confirmed the replace dialog: adopt + re-stamp. */
		bool bForce = false;
		bool bRequiresFullReload = true;
		/** Numeric uniform edits eligible for compiled dirty-cone replay. Empty
		 *  means the exact TOML must use the full reload path. */
		TArray<FParamEdit> ParamEdits;
		double Seq = -1;
		uint64 SocketId = 0;
	};
	TMap<FString, FPendingSync> PendingSyncs;

	/** Apply one staged sync (file write + reload + ack). */
	void ProcessPendingSync(FPendingSync& Pending);

	/** Apply all staged syncs now. Returns how many were processed. */
	int32 FlushPendingSyncs();


	/** User's background-CPU-throttle setting, restored after the last
	 *  client leaves. Connections are intent-driven (the editor-side
	 *  toggle), so the throttle lifts at connect — the editor is live the
	 *  moment the user establishes the link. */
	bool bSavedThrottleSetting = true;

	struct FClientState
	{
		/** Graph file paths this client synced. */
		TSet<FString> TouchedGraphs;
	};
	TMap<uint64, FClientState> Clients;
};
