/* global iceServers:readonly, Janus:readonly, server:readonly */

var janus = null;
var audiobridgeHandle = null;
var opaqueId = "transcriptiontest-"+Janus.randomString(12);

var myroom = 1234;	// Demo room
var myusername = null;
var myid = null;
var webrtcUp = false;
var ABMOD_SO_PATH = '/var/janus/janus/lib/janus/abmodules/libabmod_transcriber_template.so';
var roomBootstrapInProgress = false;
var roomBootstrapDone = false;

// STT conversation state
var sttKnownUsers = {}; // id -> display
var sttPartialItems = {}; // item_id -> { userKey, displayName, text, li }
var sttCurrentItemId = null; // Track current item_id for accumulation

// Dummy peer state
var peer1Handle = null;
var peer2Handle = null;
var peer1Muted = true;
var peer2Muted = true;

// Helper functions
function sttRememberUser(id, display) {
    if(id !== undefined && id !== null) sttKnownUsers[String(id)] = display || ("User " + id);
    if(display) sttKnownUsers[String(display)] = display;
}

function sttResolveName(userField) {
    if(userField && sttKnownUsers[String(userField)]) return sttKnownUsers[String(userField)];
    if(userField && /[^0-9]/.test(String(userField))) return String(userField);
    return null;
}

function sttResolveMultipleUsers(userField) {
    if(!userField) return null;
    
    // Check if it's a comma-separated list
    if(userField.includes(',')) {
        const userIds = userField.split(',').map(id => id.trim());
        const resolvedNames = userIds.map(id => sttResolveName(id) || id);
        return resolvedNames.join(', ');
    }
    
    // Single user
    return sttResolveName(userField);
}

function sttNowTime() {
    try {
        return new Date().toLocaleTimeString();
    } catch(e) {
        return '';
    }
}

function sttAppendMessage(user, text, isFinal) {
    if(!text || !text.trim()) return;
    const who = user || 'Unknown';
    const li = document.createElement('li');
    li.className = 'list-group-item';
    const ts = sttNowTime();
    li.innerHTML = (ts ? '<small class="text-muted">[' + escapeXmlTags(ts) + ']</small> ' : '') + 
        '<strong>' + escapeXmlTags(who) + ':</strong> ' + escapeXmlTags(text);
    if(isFinal) li.className += ' list-group-item-light';
    const ul = document.getElementById('sttConversation');
    if(ul) {
        ul.appendChild(li);
        const cont = ul.parentElement;
        if(cont && cont.scrollTo) cont.scrollTo({ top: cont.scrollHeight, behavior: 'auto' });
        else if(cont) cont.scrollTop = cont.scrollHeight;
    }
}

function sttBuildAbmodConfig() {
    // AWS example config — switch provider to 'openai' to use OpenAI Realtime instead.
    return {
        provider: 'aws',
        aws_ws_url: 'wss://transcribestreaming.us-east-1.amazonaws.com:8443/medical-stream-transcription-websocket',
        aws_language_code: 'en-US',
        aws_region: 'us-east-1',
        aws_specialty: 'PRIMARYCARE',
        aws_stream_type: 'CONVERSATION',
        aws_session_id_prefix: 'test-',
        // Optional for local testing only; prefer env vars in production.
        // aws_access_key_id: 'AKIA...',
        // aws_secret_access_key: '...',
        // aws_session_token: '...',
        aws_sigv4_expires: 300,
        aws_medical_redaction: false
    };
}

/**
 * Build an OpenAI provider config. The client owns the prompt content;
 * the server only forwards it to the OpenAI session verbatim.
 *
 * VAD params mirror the hook's TurnDetectionParams so the client
 * can tune detection independently of the server default.
 *
 * @param {object} opts
 * @param {string}  [opts.prompt]              - Transcription context prompt (client-defined)
 * @param {string}  [opts.language]            - BCP-47 language tag, e.g. 'en' (default)
 * @param {string}  [opts.model]               - OpenAI model, e.g. 'gpt-4o-transcribe'
 * @param {string}  [opts.vadType]             - 'server_vad' (default) or 'none'
 * @param {number}  [opts.vadThreshold]        - VAD threshold 0–1 (default 0.5)
 * @param {number}  [opts.vadPrefixPaddingMs]  - Prefix padding in ms (default 300)
 * @param {number}  [opts.vadSilenceDurationMs]- Silence duration in ms (default 300)
 * @param {boolean} [opts.presentationMode]    - Halves silence_duration_ms when true
 * @param {string}  [opts.noiseReduction]      - Noise reduction type, e.g. 'near_field'
 * @param {boolean} [opts.fastMode]            - Enable mini stream for low-latency partials
 * @param {number}  [opts.concurrent]          - Number of parallel main streams (default 1)
 *                                               Finals only emitted when all N agree.
 * @param {string}  [opts.apiKey]              - API key (prefer OPENAI_API_KEY env var)
 */
function sttBuildOpenAIConfig(opts) {
    opts = opts || {};
    var cfg = { provider: 'openai' };
    if(opts.apiKey)              cfg.openai_api_key = opts.apiKey;
    if(opts.model)               cfg.openai_model = opts.model;
    if(opts.language)            cfg.openai_language = opts.language;
    if(opts.prompt)              cfg.openai_prompt = opts.prompt;
    if(opts.noiseReduction)      cfg.openai_noise_reduction = opts.noiseReduction;
    if(opts.vadType)             cfg.openai_vad_type = opts.vadType;
    if(opts.vadThreshold != null)         cfg.openai_vad_threshold = opts.vadThreshold;
    if(opts.vadPrefixPaddingMs != null)   cfg.openai_vad_prefix_padding_ms = opts.vadPrefixPaddingMs;
    if(opts.vadSilenceDurationMs != null) cfg.openai_vad_silence_duration_ms = opts.vadSilenceDurationMs;
    if(opts.presentationMode)    cfg.openai_presentation_mode = true;
    if(opts.fastMode)            cfg.openai_fast_mode = true;
    if(opts.concurrent != null && opts.concurrent > 1) cfg.openai_concurrent = opts.concurrent;
    return cfg;
}

function sttUpdatePartial(itemId, userKey, displayName, text) {
    const ul = document.getElementById('sttConversation');
    if(!ul) return;
    if(!text || !text.trim()) return;
    
    const key = itemId || 'unknown_item';
    let partialItem = sttPartialItems[key];
    
    if(!partialItem) {
        const li = document.createElement('li');
        li.className = 'list-group-item list-group-item-info';
        li.style.opacity = '0.7';
        partialItem = {
            userKey: userKey,
            displayName: displayName,
            text: '',
            li: li
        };
        sttPartialItems[key] = partialItem;
        ul.appendChild(li);
    }
    
    partialItem.text = text;
    partialItem.displayName = displayName || partialItem.displayName;
    
    const ts = sttNowTime();
    partialItem.li.innerHTML = (ts ? '<small class="text-muted">[' + escapeXmlTags(ts) + ']</small> ' : '') + 
        '<strong>' + escapeXmlTags(partialItem.displayName || userKey || 'Unknown') + ':</strong> ' + 
        escapeXmlTags(text) + ' <em class="text-muted">(partial)</em>';
    
    const cont = ul.parentElement;
    if(cont && cont.scrollTo) cont.scrollTo({ top: cont.scrollHeight, behavior: 'auto' });
    else if(cont) cont.scrollTop = cont.scrollHeight;
}

function sttFinalize(itemId, userKey, displayName, text) {
    const ul = document.getElementById('sttConversation');
    if(!ul) return;
    
    const key = itemId || 'unknown_item';
    const partialItem = sttPartialItems[key];
    
    const finalText = (text && text.trim()) ? text : (partialItem ? partialItem.text : '');
    
    if(partialItem && partialItem.li) {
        try { 
            ul.removeChild(partialItem.li); 
        } catch(e) {}
        delete sttPartialItems[key];
    }
    
    if(finalText) {
        sttAppendMessage(displayName || (partialItem ? partialItem.displayName : userKey) || 'Unknown', finalText, true);
    }
}

function escapeXmlTags(value) {
    if(value) {
        let escapedValue = value.replace(new RegExp('<', 'g'), '&lt');
        escapedValue = escapedValue.replace(new RegExp('>', 'g'), '&gt');
        return escapedValue;
    }
}

// Load audio files and create audio context for dummy peers
async function loadAudioForPeer(fileName, peerNumber) {
    return new Promise((resolve, reject) => {
        const audio = new Audio(fileName);
        audio.loop = true;
        audio.preload = 'auto';
        
        audio.addEventListener('loadeddata', () => {
            console.log('Audio loaded for peer ' + peerNumber);
            resolve(audio);
        });
        
        audio.addEventListener('error', (e) => {
            console.error('Error loading audio for peer ' + peerNumber, e);
            reject(e);
        });
        
        audio.load();
    });
}

// Shared Web Audio mixer — one permanent track sent to Janus.
// Peer1/Peer2 are gain nodes; set gain to 1 to unmute, 0 to mute.
// No track replacement or renegotiation needed.
var audioContext = null;
var mixerDestination = null;  // MediaStreamDestination — its track is the Janus sender
var peer1Source = null;       // MediaElementAudioSourceNode for peer1
var peer2Source = null;       // MediaElementAudioSourceNode for peer2
var peer1GainNode = null;
var peer2GainNode = null;

function getAudioContext() {
    if(!audioContext)
        audioContext = new (window.AudioContext || window.webkitAudioContext)();
    return audioContext;
}

function getMixerTrack() {
    const ctx = getAudioContext();
    if(!mixerDestination)
        mixerDestination = ctx.createMediaStreamDestination();
    return mixerDestination.stream.getAudioTracks()[0];
}

function setUiJoinedState(joined) {
    // Keep unload disabled until ABMod is explicitly loaded.
    $('#loadabmod').prop('disabled', !joined);
    $('#togglePeer1').prop('disabled', !joined);
    $('#togglePeer2').prop('disabled', !joined);
    if(!joined) {
        $('#unloadabmod').prop('disabled', true);
    }
}

// Wire an audio element into the shared mixer with its own gain node.
// Returns the gain node so the caller can mute/unmute by setting gain.value.
function connectAudioElementToMixer(audioElement) {
    const ctx = getAudioContext();
    if(!mixerDestination)
        mixerDestination = ctx.createMediaStreamDestination();
    const source = ctx.createMediaElementSource(audioElement);
    const gain = ctx.createGain();
    gain.gain.value = 0; // start silent
    source.connect(gain);
    gain.connect(mixerDestination);
    console.log('Connected audio element to mixer:', audioElement.src);
    return gain;
}

$(document).ready(function() {
    // Guard controls until we are actually joined in a room.
    setUiJoinedState(false);

    // Initialize Janus library
    Janus.init({debug: "all", callback: function() {
        $('#start').one('click', function() {
            $(this).attr('disabled', true).unbind('click');

            // Create (and immediately resume) the AudioContext inside the user
            // gesture handler. Chrome suspends AudioContexts created outside of
            // a direct user gesture, which would silently produce an empty track
            // and send no RTP to Janus.
            const ctx = getAudioContext();
            ctx.resume().then(() => {
                console.log('AudioContext state after resume:', ctx.state);
            });

            // Load audio files
            Promise.all([
                loadAudioForPeer('LJ001-0001.wav', 1),
                loadAudioForPeer('LJ001-0003.wav', 2)
            ]).then(([audio1, audio2]) => {
                // Setup audio elements
                $('#peer1Audio').get(0).srcObject = null;
                $('#peer1Audio').get(0).src = 'LJ001-0001.wav';
                $('#peer2Audio').get(0).srcObject = null;
                $('#peer2Audio').get(0).src = 'LJ001-0003.wav';
                
                // Create Janus session
                janus = new Janus({
                    server: server,
                    iceServers: iceServers,
                    success: function() {
                        // Attach to AudioBridge plugin
                        janus.attach({
                            plugin: "janus.plugin.audiobridge",
                            opaqueId: opaqueId,
                            success: function(pluginHandle) {
                                audiobridgeHandle = pluginHandle;
                                console.log("Plugin attached!");
                                $('#details').remove();
                                $('#connected').removeClass('hide');
                                
                                // Ensure test room exists, then join
                                ensureRoomThenJoin();
                            },
                            error: function(error) {
                                console.error("Error attaching plugin...", error);
                                bootbox.alert("Error attaching plugin... " + error);
                                $('#start').removeAttr('disabled').html("Start");
                            },
                            consentDialog: function(on) {
                                console.log("Consent dialog should be " + (on ? "on" : "off") + " now");
                            },
                            iceState: function(state) {
                                console.log("ICE state changed to " + state);
                            },
                            mediaState: function(medium, on, mid) {
                                console.log("Janus " + (on ? "started" : "stopped") + " receiving our " + medium + " (mid=" + mid + ")");
                            },
                            webrtcState: function(on) {
                                console.log("Janus says our WebRTC PeerConnection is " + (on ? "up" : "down") + " now");
                            },
                            onmessage: function(msg, jsep) {
                                console.log(" ::: Got a message :::", msg);
                                handleMessage(msg, jsep);
                            },
                            onlocaltrack: function(track, on) {
                                console.log("Local track " + (on ? "added" : "removed") + ":", track);
                            },
                            onremotetrack: function(track, mid, on, metadata) {
                                console.log("Remote track " + (on ? "added" : "removed") + ":", track);
                            },
                            oncleanup: function() {
                                console.log(" ::: Got a cleanup notification :::");
                            }
                        });
                    },
                    error: function(error) {
                        console.error(error);
                        bootbox.alert(error, function() {
                            window.location.reload();
                        });
                    },
                    destroyed: function() {
                        window.location.reload();
                    }
                });
            }).catch((error) => {
                console.error('Error loading audio files:', error);
                bootbox.alert("Error loading audio files. Please make sure peer1.wav and peer2.wav are in the same directory as this HTML file.");
                $('#start').removeAttr('disabled').html("Start");
            });
            
            $('#start').click(function() {
                $(this).attr('disabled', true);
                janus.destroy();
            });
        });
        
        // ABMod buttons
        $(document).on('click', '#loadabmod', function() {
            if(!myid) {
                console.log('Join is not complete yet, skipping ABMod load.');
                return;
            }
            const cfg = sttBuildAbmodConfig();
            audiobridgeHandle.send({ message: { request: 'configure', abmod_load: ABMOD_SO_PATH, abmod_config: JSON.stringify(cfg) } });
            $(this).prop('disabled', true);
            $('#unloadabmod').prop('disabled', false);
        });
        
        $(document).on('click', '#unloadabmod', function() {
            if(!myid) {
                console.log('Join is not complete yet, skipping ABMod unload.');
                return;
            }
            audiobridgeHandle.send({ message: { request: 'configure', abmod_unload: true } });
            $(this).prop('disabled', true);
            $('#loadabmod').prop('disabled', false);
        });
        
        // Dummy peer mute/unmute buttons
        $(document).on('click', '#togglePeer1', function() {
            if(peer1Muted) {
                // Unmute
                startPeer1Audio();
                $(this).html('Mute (Stop Audio)').removeClass('btn-success').addClass('btn-danger');
                $('#peer1Status').text('Playing');
            } else {
                // Mute
                stopPeer1Audio();
                $(this).html('Unmute (Start Audio)').removeClass('btn-danger').addClass('btn-success');
                $('#peer1Status').text('Stopped');
            }
        });
        
        $(document).on('click', '#togglePeer2', function() {
            if(peer2Muted) {
                // Unmute
                startPeer2Audio();
                $(this).html('Mute (Stop Audio)').removeClass('btn-success').addClass('btn-danger');
                $('#peer2Status').text('Playing');
            } else {
                // Mute
                stopPeer2Audio();
                $(this).html('Unmute (Start Audio)').removeClass('btn-danger').addClass('btn-success');
                $('#peer2Status').text('Stopped');
            }
        });
    }});
});

function joinRoom() {
    myusername = "test-user-" + Janus.randomString(8);
    let register = { request: "join", room: myroom, display: myusername };
    audiobridgeHandle.send({
        message: register,
        success: function(reply) {
            // Some Janus paths return join data in the direct success callback.
            if(reply && Number(reply["room"]) === Number(myroom) && reply["id"]) {
                myid = reply["id"];
                console.log("Joined room via success callback " + reply["room"] + " with ID " + myid);
                setUiJoinedState(true);
                if(!webrtcUp) {
                    webrtcUp = true;
                    setupWebRTC();
                }
            }
        },
        error: function(err) {
            console.error("Join request failed:", err);
        }
    });
}

function ensureRoomThenJoin() {
    if(roomBootstrapDone) {
        joinRoom();
        return;
    }
    if(roomBootstrapInProgress) {
        return;
    }
    roomBootstrapInProgress = true;
    const create = {
        request: "create",
        room: myroom,
        description: "Transcription Test Room",
        is_private: false,
        sampling_rate: 16000,
        permanent: false
    };
    console.log("Ensuring room exists:", myroom);
    audiobridgeHandle.send({
        message: create,
        success: function(reply) {
            // Handle both created and already-exists in direct callback path.
            if(reply && (reply["audiobridge"] === "created" || isRoomAlreadyExistsError(reply))) {
                console.log("Room bootstrap success callback:", reply);
                finalizeRoomBootstrapAndJoin();
                return;
            }
            // If no explicit create result is returned, still continue with join.
            console.log("Room bootstrap callback without explicit status, attempting join:", reply);
            finalizeRoomBootstrapAndJoin();
        },
        error: function(err) {
            if(isRoomAlreadyExistsError(err)) {
                console.log("Room already exists via error callback:", myroom);
                finalizeRoomBootstrapAndJoin();
                return;
            }
            console.error("Room create failed:", err);
            roomBootstrapInProgress = false;
            bootbox.alert("Failed to create/join test room " + myroom + ": " + (err && (err.error || err.message) ? (err.error || err.message) : "unknown error"));
        }
    });
}

function finalizeRoomBootstrapAndJoin() {
    roomBootstrapInProgress = false;
    roomBootstrapDone = true;
    joinRoom();
}

function isRoomAlreadyExistsError(msg) {
    if(!msg) return false;
    const code = Number(msg["error_code"]);
    if(code === 486) return true;
    const err = String(msg["error"] || "").toLowerCase();
    return err.includes("already exists");
}

function handleMessage(msg, jsep) {
    let event = msg["audiobridge"];
    console.log("Event: " + event);
    
    if(event) {
        // Handle room bootstrap responses (create room before join)
        if(roomBootstrapInProgress) {
            if(event === "created" && Number(msg["room"]) === Number(myroom)) {
                console.log("Room created:", myroom);
                finalizeRoomBootstrapAndJoin();
                return;
            }
            if(event === "event" && isRoomAlreadyExistsError(msg)) {
                // Room already exists: proceed with join.
                console.log("Room already exists:", myroom);
                finalizeRoomBootstrapAndJoin();
                return;
            }
            if(event === "event" && msg["error"]) {
                console.warn("Room bootstrap response:", msg["error"]);
            }
        }

        // Handle ABMod transcription events
        if(event === "abmod") {
            let sttType = msg["event"];
            let payload = msg["payload"] || {};
            let userField = payload.user || payload.user_id || "";
            let itemId = payload.item_id || null;
            let language = payload.language || '';
            let provider = payload.provider || '';
            let payloadType = payload.type || '';
            let transcriptText = payload.text || payload.transcript || payload.message || '';
            let who = sttResolveMultipleUsers(userField) || "Multiple Speakers";
            
            $("#sttStatus").text(sttType === "transcription.final" ? "final" : "listening…");
            // Use the original user field (may be comma-separated) as the key
            const userKey = String(userField || who);
            const labelPrefix = (provider || language) ? ("[" + [provider, language].filter(Boolean).join("/") + "] ") : "";
            
            if(payloadType === 'partial' || sttType === 'transcription') {
                sttUpdatePartial(itemId, userKey, who, transcriptText);
            } else if(payloadType === 'final' || sttType === 'transcription.final') {
                sttFinalize(itemId, userKey, who, transcriptText);
            } else if(sttType === 'error') {
                const isAuthError = payloadType === 'auth_error';
                if(transcriptText) sttAppendMessage(labelPrefix + who, (isAuthError ? '[auth_error] ' : '[error] ') + transcriptText, true);
            }
            return;
        }
        
        // Handle joined event
        if(event === "joined") {
            if(msg["id"]) {
                myid = msg["id"];
                console.log("Successfully joined room " + msg["room"] + " with ID " + myid);
                setUiJoinedState(true);
                if(!webrtcUp) {
                    webrtcUp = true;
                    setupWebRTC();
                }
            }
        }

        // Some Janus/AudioBridge flows may signal room/user info in generic events.
        if(!myid && msg["id"] && Number(msg["room"]) === Number(myroom)) {
            myid = msg["id"];
            console.log("Join inferred from event payload for room " + msg["room"] + " with ID " + myid);
            setUiJoinedState(true);
            if(!webrtcUp) {
                webrtcUp = true;
                setupWebRTC();
            }
        }
        
        // Handle error
        if(event === "event" && msg["error"]) {
            console.error("Error: " + msg["error"]);
            if(isRoomAlreadyExistsError(msg)) {
                // Create request raced with existing room: continue test flow.
                if(!roomBootstrapDone) {
                    console.log("Room already exists (non-bootstrap path):", myroom);
                    finalizeRoomBootstrapAndJoin();
                } else if(!myid) {
                    joinRoom();
                }
                return;
            }
            if(Number(msg["error_code"]) === 485) {
                // Retry once by creating room dynamically in test flow.
                if(!roomBootstrapDone) {
                    ensureRoomThenJoin();
                    return;
                }
                bootbox.alert("Room does not exist and dynamic create failed for room " + myroom);
            } else if(Number(msg["error_code"]) === 487) {
                // Harmless race if a configure request is attempted too early.
                console.warn("Configure requested before join completed; waiting for joined event.");
                return;
            }
            return;
        }
    }
    
    if(jsep) {
        console.log("Handling SDP...", jsep);
        audiobridgeHandle.handleRemoteJsep({ jsep: jsep });
    }
}

function setupWebRTC() {
    // Pass the permanent mixer track as the audio sender.
    // Peer gain nodes control what flows into it — no renegotiation needed.
    const mixerTrack = getMixerTrack();
    audiobridgeHandle.createOffer({
        tracks: [
            { type: 'audio', capture: false, recv: true, add: mixerTrack },
        ],
        success: function(jsep) {
            console.log("Got SDP!", jsep);
            // muted:false — Janus should forward whatever the mixer sends
            audiobridgeHandle.send({ message: { request: "configure", muted: false }, jsep: jsep });
        },
        error: function(error) {
            console.error("WebRTC error:", error);
            bootbox.alert("WebRTC error... " + error.message);
        }
    });
}

function startPeer1Audio() {
    getAudioContext().resume();
    const audioElement = document.getElementById('peer1Audio');
    if(!peer1GainNode) {
        peer1GainNode = connectAudioElementToMixer(audioElement);
    }
    peer1GainNode.gain.value = 1;
    audioElement.play().catch(e => console.error('Error playing peer1 audio:', e));
    console.log('Peer1 unmuted, AudioContext state:', getAudioContext().state);
    peer1Muted = false;
}

function stopPeer1Audio() {
    if(peer1GainNode) peer1GainNode.gain.value = 0;
    document.getElementById('peer1Audio').pause();
    peer1Muted = true;
}

function startPeer2Audio() {
    getAudioContext().resume();
    const audioElement = document.getElementById('peer2Audio');
    if(!peer2GainNode) {
        peer2GainNode = connectAudioElementToMixer(audioElement);
    }
    peer2GainNode.gain.value = 1;
    audioElement.play().catch(e => console.error('Error playing peer2 audio:', e));
    console.log('Peer2 unmuted, AudioContext state:', getAudioContext().state);
    peer2Muted = false;
}

function stopPeer2Audio() {
    if(peer2GainNode) peer2GainNode.gain.value = 0;
    document.getElementById('peer2Audio').pause();
    peer2Muted = true;
}

