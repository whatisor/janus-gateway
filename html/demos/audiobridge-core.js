// audiobridge-core.js
// Core API for Janus audiobridge demo (no UI, no chat, no DOM)

export default class AudiobridgeCore {
  constructor({ server, iceServers, room = 1234, acodec = null, stereo = false, group = null, suspended = false, onJoined, onParticipants, onRoomChanged, onDestroyed, onError, onRemoteTrack, onLocalTrack, onCleanup, onMute, onSuspend, onResume, onSpatialPosition } = {}) {
    this.server = server;
    this.iceServers = iceServers;
    this.room = room;
    this.acodec = acodec;
    this.stereo = stereo;
    this.group = group;
    this.suspended = suspended;
    this.onJoined = onJoined;
    this.onParticipants = onParticipants;
    this.onRoomChanged = onRoomChanged;
    this.onDestroyed = onDestroyed;
    this.onError = onError;
    this.onRemoteTrack = onRemoteTrack;
    this.onLocalTrack = onLocalTrack;
    this.onCleanup = onCleanup;
    this.onMute = onMute;
    this.onSuspend = onSuspend;
    this.onResume = onResume;
    this.onSpatialPosition = onSpatialPosition;
    this.janus = null;
    this.pluginHandle = null;
    this.myid = null;
    this.username = null;
    this.webrtcUp = false;
    this.audioenabled = false;
    this.audiosuspended = suspended;
  }

  initJanus(JanusLib, opaqueId) {
    return new Promise((resolve, reject) => {
      JanusLib.init({debug: "all", callback: () => {
        this.janus = new JanusLib({
          server: this.server,
          iceServers: this.iceServers,
          success: () => {
            this.janus.attach({
              plugin: "janus.plugin.audiobridge",
              opaqueId,
              success: (pluginHandle) => {
                this.pluginHandle = pluginHandle;
                resolve();
              },
              error: (error) => {
                if(this.onError) this.onError(error);
                reject(error);
              },
              consentDialog: () => {},
              iceState: () => {},
              mediaState: () => {},
              webrtcState: () => {},
              onmessage: (msg, jsep) => this.handleMessage(msg, jsep),
              onlocaltrack: (track, on) => { if(this.onLocalTrack) this.onLocalTrack(track, on); },
              onremotetrack: (track, mid, on, metadata) => { if(this.onRemoteTrack) this.onRemoteTrack(track, mid, on, metadata); },
              oncleanup: () => { if(this.onCleanup) this.onCleanup(); }
            });
          },
          error: (error) => { if(this.onError) this.onError(error); reject(error); },
          destroyed: () => { if(this.onDestroyed) this.onDestroyed(); }
        });
      }});
    });
  }

  join(username) {
    this.username = username;
    let register = { request: "join", room: this.room, display: username, suspended: this.audiosuspended };
    if(this.acodec) register.codec = this.acodec;
    if(this.group) register.group = this.group;
    this.pluginHandle.send({ message: register });
  }

  mute(muted) {
    this.audioenabled = !muted;
    this.pluginHandle.send({ message: { request: "configure", muted } });
    if(this.onMute) this.onMute(muted);
  }

  suspend(suspend) {
    this.audiosuspended = suspend;
    this.pluginHandle.send({ message: { request: suspend ? "suspend" : "resume", room: this.room, id: this.myid } });
    if(suspend && this.onSuspend) this.onSuspend();
    if(!suspend && this.onResume) this.onResume();
  }

  setSpatialPosition(position) {
    this.pluginHandle.send({ message: { request: "configure", spatial_position: position } });
    if(this.onSpatialPosition) this.onSpatialPosition(position);
  }

  handleMessage(msg, jsep) {
    const event = msg["audiobridge"];
    if(event === "joined") {
      this.myid = msg["id"];
      if(this.onJoined) this.onJoined(msg);
      if(jsep) this.pluginHandle.handleRemoteJsep({ jsep });
      if(msg["participants"] && this.onParticipants) this.onParticipants(msg["participants"]);
    } else if(event === "roomchanged") {
      this.myid = msg["id"];
      if(this.onRoomChanged) this.onRoomChanged(msg);
      if(msg["participants"] && this.onParticipants) this.onParticipants(msg["participants"]);
    } else if(event === "destroyed") {
      if(this.onDestroyed) this.onDestroyed();
    } else if(event === "event") {
      if(msg["participants"] && this.onParticipants) this.onParticipants(msg["participants"]);
      if(msg["leaving"] && this.onParticipants) this.onParticipants([]); // Could be improved
      if(msg["suspended"] && this.onSuspend) this.onSuspend(msg["suspended"]);
      if(msg["resumed"] && this.onResume) this.onResume(msg["resumed"]);
      if(msg["error"] && this.onError) this.onError(msg["error"]);
    }
  }
} 