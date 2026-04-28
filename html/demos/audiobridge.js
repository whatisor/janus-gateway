/* global server:readonly, iceServers:readonly, Janus:readonly */

var janus = null;
var audiobridgeHandle = null;
var opaqueId = "audiobridge-" + Janus.randomString(12);

var myroom = 1234;
var myusername = null;
var myid = null;
var webrtcUp = false;

var ABMOD_SO_PATH = '/var/janus/janus/lib/janus/abmodules/libabmod_transcriber_template.so';

// STT display state
var sttPartialItems = {};
var sttKnownUsers  = {};
var sttTalkingUsers = {};

// ─── STT helpers ────────────────────────────────────────────────────────────

function sttRememberUser(id, display) {
	if(id !== undefined && id !== null) sttKnownUsers[String(id)] = display || ("User " + id);
	if(display) sttKnownUsers[String(display)] = display;
}

function sttResolveName(u) {
	if(u && sttKnownUsers[String(u)]) return sttKnownUsers[String(u)];
	if(u && /[^0-9]/.test(String(u))) return String(u);
	return null;
}

function sttResolveMultiple(u) {
	if(!u) return null;
	if(String(u).includes(','))
		return String(u).split(',').map(x => sttResolveName(x.trim()) || x.trim()).join(', ');
	return sttResolveName(u);
}

function sttResolveId(raw) {
	if(raw === undefined || raw === null) return null;
	return String(raw);
}

function sttSetActivityStatus(text) {
	var el = $('#talkingStatus');
	if(!el.length) el = $('#sttStatus');
	if(el.length) el.text(text || 'idle');
}

function sttSetParticipantTalking(userId, display, isTalking) {
	var id = sttResolveId(userId);
	if(!id) return;
	if(display) sttRememberUser(id, display);
	if(isTalking) sttTalkingUsers[id] = true;
	else delete sttTalkingUsers[id];
	var row = $('#rp' + id);
	if(row.length) {
		row.toggleClass('list-group-item-success', !!isTalking);
		row.find('.rp-talking').toggleClass('d-none', !isTalking);
	}
	var names = Object.keys(sttTalkingUsers).map(function(uid) {
		return sttResolveName(uid) || uid;
	});
	if(names.length > 0)
		sttSetActivityStatus('talking: ' + names.join(', '));
	else
		sttSetActivityStatus('idle');
}

function escapeHtml(v) {
	if(!v) return '';
	return String(v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function sttFormatGenderTag(payload) {
	if(!payload || !payload.gender) return '';
	var label = String(payload.gender);
	var conf = Number(payload.gender_confidence);
	if(Number.isFinite(conf))
		return ' <span class="text-muted">(' + escapeHtml(label) + ' ' + Math.round(conf * 100) + '%)</span>';
	return ' <span class="text-muted">(' + escapeHtml(label) + ')</span>';
}

function sttUpdatePartial(itemId, userKey, displayName, text, payload) {
	var ul = document.getElementById('sttConversation');
	if(!ul || !text || !text.trim()) return;
	var key = itemId || 'unknown';
	var item = sttPartialItems[key];
	var genderTag = sttFormatGenderTag(payload);
	if(!item) {
		var li = document.createElement('li');
		li.className = 'list-group-item list-group-item-info';
		li.style.opacity = '0.7';
		item = { li: li, displayName: displayName };
		sttPartialItems[key] = item;
		ul.appendChild(li);
	}
	item.displayName = displayName || item.displayName;
	item.li.innerHTML = '<strong>' + escapeHtml(item.displayName || userKey) + ':</strong> ' +
		escapeHtml(text) + genderTag + ' <em class="text-muted">(partial)</em>';
	ul.parentElement.scrollTop = ul.parentElement.scrollHeight;
}

function sttFinalize(itemId, userKey, displayName, text, payload) {
	var ul = document.getElementById('sttConversation');
	if(!ul) return;
	var key = itemId || 'unknown';
	var item = sttPartialItems[key];
	var finalText = (text && text.trim()) ? text : (item ? item.text : '');
	var genderTag = sttFormatGenderTag(payload);
	if(item && item.li) { try { ul.removeChild(item.li); } catch(e) {} delete sttPartialItems[key]; }
	if(!finalText) return;
	var li = document.createElement('li');
	li.className = 'list-group-item list-group-item-light';
	var ts = new Date().toLocaleTimeString();
	li.innerHTML = '<small class="text-muted">[' + ts + ']</small> ' +
		'<strong>' + escapeHtml(displayName || userKey || 'Unknown') + ':</strong> ' + escapeHtml(finalText) + genderTag;
	ul.appendChild(li);
	ul.parentElement.scrollTop = ul.parentElement.scrollHeight;
}

function sttAppendError(who, msg) {
	var ul = document.getElementById('sttConversation');
	if(!ul) return;
	var li = document.createElement('li');
	li.className = 'list-group-item list-group-item-danger';
	li.innerHTML = '<strong>' + escapeHtml(who || 'STT') + ':</strong> ' + escapeHtml(msg);
	ul.appendChild(li);
	ul.parentElement.scrollTop = ul.parentElement.scrollHeight;
}

function sttNormalizeAbmod(msg) {
	var payload = msg && (msg.payload || msg.data) ? (msg.payload || msg.data) : {};
	var sttEvent = (msg && (msg.event || msg.type)) || payload.event || '';
	var payloadType = payload.type || '';
	var userField = payload.user_id || payload.userId || payload.user || '';
	var itemId = payload.item_id || payload.itemId || payload.id || null;
	var text = payload.text || payload.message || '';
	var provider = payload.provider || '';
	var language = payload.language || payload.lang || '';
	return {
		event: sttEvent,
		type: payloadType,
		userField: userField,
		itemId: itemId,
		text: text,
		provider: provider,
		language: language,
		payload: payload
	};
}

// ─── ABMod config ────────────────────────────────────────────────────────────

function generateUUID() {
	return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
		var r = Math.random() * 16 | 0;
		return (c === 'x' ? r : (r & 0x3 | 0x8)).toString(16);
	});
}

function buildAbmodConfig() {
	var provider = $('#sttProvider').val() || 'aws';
	var genderEnabled = $('#genderEnabled').length ? $('#genderEnabled').is(':checked') : true;
	if(provider === 'openai') {
		var cfg = {
			provider: 'openai',
			openai_model: $('#openaiModel').val() || 'gpt-4o-transcribe',
			openai_language: 'en',
			gender_enabled: genderEnabled
		};
		var concurrent = parseInt($('#openaiConcurrent').val(), 10);
		if(concurrent > 1) cfg.openai_concurrent = concurrent;
		if($('#openaiFastMode').is(':checked')) cfg.openai_fast_mode = true;
		return cfg;
	}
	var awsCfg = {
		provider: 'aws',
		gender_enabled: genderEnabled,
		aws_language_code: 'en-US',
		aws_region: 'us-east-1',
		aws_specialty: 'PRIMARYCARE',
		aws_stream_type: 'CONVERSATION',
		aws_session_id: generateUUID(),
		aws_medical_redaction: false,
		aws_fast_mode: $('#awsFastMode').is(':checked'),
		aws_vocabulary_name:'test'
	};
	var vocabPrompt = $('#awsVocabPrompt').val().trim();
	if(vocabPrompt) awsCfg.aws_vocabulary_prompt = vocabPrompt;
	return awsCfg;
}

// ─── Room bootstrap ───────────────────────────────────────────────────────────

function doJoin() {
	audiobridgeHandle.send({
		message: { request: 'join', room: myroom, display: myusername }
	});
}

function createRoomThenJoin(options) {
	options = options || {};
	audiobridgeHandle.send({
		message: {
			request: 'create',
			room: myroom,
			description: 'AudioBridge room ' + myroom,
			is_private: false,
			denoise: options.denoise !== undefined ? options.denoise : false,
			audiolevel_ext: options.audiolevel_ext !== undefined ? options.audiolevel_ext : true,
			audiolevel_event: options.audiolevel_event !== undefined ? options.audiolevel_event : true,
			audio_active_packets: options.audio_active_packets || 20,
			audio_level_average: options.audio_level_average || 35,
			sampling_rate: options.sampling_rate,
			secret: options.secret,
			use_limiter: options.use_limiter !== undefined ? !!options.use_limiter : true,
			volume: options.volume ?? 90,
			permanent: false
		},
		success: function(result) {
			Janus.log('Room create result:', result);
			doJoin();
		},
		error: function(err) {
			// 486 = room already exists — race condition, just join anyway
			if(err && (Number(err['error_code']) === 486 || String(err).toLowerCase().includes('already exists'))) {
				Janus.log('Room already exists, joining...');
				doJoin();
				return;
			}
			Janus.error('Failed to create room:', err);
			bootbox.alert('Failed to create room ' + myroom + ': ' + (err['error'] || err));
			$('#registernow').removeClass('hide');
		}
	});
}

// ─── Message handler ─────────────────────────────────────────────────────────

function handleMessage(msg, jsep) {
	var event = msg['audiobridge'];
	Janus.log('AudioBridge event:', event, msg);

	if(event === 'joined') {
		myid = msg['id'];
		myusername = msg['display'];
		sttRememberUser(myid, myusername);
		Janus.log('Joined room', myroom, 'as', myid);
		$('#you').text('You: ' + myusername).removeClass('hide');
		$('#toggleaudio').removeClass('hide');
		$('#togglesuspend').removeClass('hide');
		$('#loadabmod').prop('disabled', false);
		// Build participant list from existing members
		var list = msg['participants'];
		if(list && list.length > 0) {
			list.forEach(function(p) {
				sttRememberUser(p['id'], p['display']);
				addParticipant(p['id'], p['display'], p['muted']);
			});
		}
		if(!webrtcUp) {
			webrtcUp = true;
			setupWebRTC();
		}
	}

	if(event === 'roomchanged') {
		myid = msg['id'];
		$('#list').empty();
	}

	if(event === 'destroyed') {
		bootbox.alert('Room destroyed!', function() { window.location.reload(); });
	}

	if(event === 'event') {
		// Participant list changes
		var participants = msg['participants'];
		if(participants && participants.length > 0) {
			participants.forEach(function(p) {
				sttRememberUser(p['id'], p['display']);
				if($('#rp' + p['id']).length === 0)
					addParticipant(p['id'], p['display'], p['muted']);
				else
					$('#rp' + p['id'] + ' i').toggleClass('fa-microphone', !p['muted']).toggleClass('fa-microphone-slash', !!p['muted']);
				if(p['talking'] !== undefined)
					sttSetParticipantTalking(p['id'], p['display'], !!p['talking']);
			});
		}
		var leaving = msg['leaving'];
		if(leaving) {
			sttSetParticipantTalking(leaving, null, false);
			removeParticipant(leaving);
		}
		var kicked = msg['kicked'];
		if(kicked) {
			sttSetParticipantTalking(kicked, null, false);
			removeParticipant(kicked);
		}
		var error = msg['error'];
		var errorCode = Number(msg['error_code']);
		if(error) {
			Janus.error('AudioBridge error ' + errorCode + ':', error);
			if(errorCode === 485) {
				// Room does not exist — create it then retry join
				Janus.log('Room', myroom, 'not found, creating it...');
				createRoomThenJoin();
				return;
			}
			bootbox.alert(error);
		}
		// Mute toggle confirmation
		if(msg['result'] && msg['result'] === 'ok') { /* configure ack */ }
	}

	if(event === 'talking' || event === 'stopped-talking') {
		var speaking = event === 'talking';
		var talkId = msg['id'] !== undefined ? msg['id'] : (msg['user_id'] !== undefined ? msg['user_id'] : msg['user']);
		var talkDisplay = msg['display'];
		sttSetParticipantTalking(talkId, talkDisplay, speaking);
	}

	// ABMod transcription events
	// ABMod emits exactly two event types:
	//   event='transcription'  payload.type='partial'|'final'  payload.item_id=<aws-result-uuid>
	//   event='error'          payload.type='error'|'auth_error'
	if(event === 'abmod') {
		var stt = sttNormalizeAbmod(msg);
		var sttType = stt.event;
		var payload = stt.payload;
		var userField = stt.userField;
		var itemId = stt.itemId;
		var text = stt.text;
		var payloadType = stt.type;
		var who = sttResolveMultiple(userField) || 'Unknown';
		if(userField && who)
			sttRememberUser(userField, who);

		if(sttType === 'transcription' || sttType === 'transcript') {
			$('#sttStatus').text(payloadType === 'final' ? 'final' : 'listening\u2026');
			if(payloadType === 'partial') {
				sttUpdatePartial(itemId, String(userField), who, text, payload);
			} else if(payloadType === 'final' || !payloadType) {
				/* Treat missing type as finalized text for compatibility with older emitters. */
				sttFinalize(itemId, String(userField), who, text, payload);
			}
		} else if(sttType === 'error') {
			$('#sttStatus').text('error');
			var errTag = payloadType ? ('[' + payloadType + '] ') : '';
			var whoTag = who ? who : (payload.provider || 'STT');
			sttAppendError(errTag + whoTag, text || 'Unknown STT error');
		}
		return;
	}

	if(jsep) {
		Janus.log('Handling SDP:', jsep);
		audiobridgeHandle.handleRemoteJsep({ jsep: jsep });
	}
}

// ─── Participant list ─────────────────────────────────────────────────────────

function addParticipant(id, display, muted) {
	if($('#rp' + id).length > 0) return;
	var icon = muted ? 'fa-microphone-slash' : 'fa-microphone';
	var li = $('<li id="rp' + id + '" class="list-group-item">' +
		'<i class="fa-solid ' + icon + ' me-2"></i>' + escapeHtml(display || id) +
		'<span class="badge text-bg-success float-end rp-talking d-none">talking</span></li>');
	$('#list').append(li);
}

function removeParticipant(id) {
	$('#rp' + id).remove();
}

// ─── WebRTC ───────────────────────────────────────────────────────────────────

function setupWebRTC() {
	audiobridgeHandle.createOffer({
		tracks: [{ type: 'audio', capture: true, recv: true }],
		success: function(jsep) {
			Janus.log('Got SDP:', jsep);
			audiobridgeHandle.send({ message: { request: 'configure', muted: false }, jsep: jsep });
		},
		error: function(error) {
			Janus.error('WebRTC error:', error);
			bootbox.alert('WebRTC error: ' + error.message);
		}
	});
}

// ─── Document ready ───────────────────────────────────────────────────────────

$(document).ready(function() {
	$('#room').addClass('hide');
	$('#audiojoin').addClass('hide');

	Janus.init({ debug: 'all', callback: function() {
		$('#start').one('click', function() {
			$(this).attr('disabled', true);
			if(!Janus.isWebrtcSupported()) {
				bootbox.alert('No WebRTC support in this browser!');
				return;
			}
			janus = new Janus({
				server: server,
				iceServers: iceServers,
				success: function() {
					janus.attach({
						plugin: 'janus.plugin.audiobridge',
						opaqueId: opaqueId,
						success: function(handle) {
							audiobridgeHandle = handle;
							$('#details').addClass('hide');
							$('#audiojoin').removeClass('hide');
							$('#registernow').removeClass('hide');
						},
						error: function(err) {
							Janus.error('Error attaching:', err);
							bootbox.alert('Error attaching to AudioBridge: ' + err);
						},
						iceState: function(state) { Janus.log('ICE state:', state); },
						mediaState: function(medium, on) { Janus.log('Janus', on ? 'started' : 'stopped', 'receiving', medium); },
						webrtcState: function(on) {
							Janus.log('WebRTC PeerConnection is', on ? 'up' : 'down');
							$('#toggleaudio').prop('disabled', !on);
						},
						onmessage: function(msg, jsep) { handleMessage(msg, jsep); },
						onlocaltrack: function(track, on) { Janus.log('Local track', on ? 'added' : 'removed', track); },
						onremotetrack: function(track, mid, on) {
							Janus.log('Remote track', on ? 'added' : 'removed', track);
							if(track.kind !== 'audio') return;
							var audio = $('#mixedaudio audio').get(0);
							if(!on) { if(audio) audio.srcObject = null; return; }
							if(!audio) {
								audio = document.createElement('audio');
								audio.autoplay = true;
								audio.controls = true;
								audio.style.width = '100%';
								$('#mixedaudio').append(audio);
							}
							var stream = audio.srcObject;
							if(!stream) { stream = new MediaStream(); audio.srcObject = stream; }
							stream.addTrack(track);
							audio.play().catch(function(e) { Janus.warn('Remote audio autoplay blocked:', e); });
						},
						oncleanup: function() {
							Janus.log('Cleanup');
							webrtcUp = false;
							myid = null;
							$('#room').addClass('hide');
							$('#audiojoin').removeClass('hide');
						}
					});
				},
				error: function(err) { Janus.error(err); bootbox.alert(err, function() { window.location.reload(); }); },
				destroyed: function() { window.location.reload(); }
			});
		});
	}});

	// Join — try to join; if room missing, auto-create then retry
	$('#register').on('click', function() {
		var username = $('#username').val().trim();
		if(!username) { bootbox.alert('Enter a display name'); return; }
		myusername = username;
		$('#registernow').addClass('hide');
		$('#audiojoin').addClass('hide');
		$('#room').removeClass('hide');
		doJoin();
	});

	// Enter key in username field
	$('#username').on('keypress', function(e) {
		if(e.which === 13) $('#register').trigger('click');
	});

	// Mute/unmute own mic
	$('#toggleaudio').on('click', function() {
		var muted = audiobridgeHandle.isAudioMuted();
		if(muted) { audiobridgeHandle.unmuteAudio(); $(this).text('Mute').removeClass('btn-success').addClass('btn-danger'); }
		else       { audiobridgeHandle.muteAudio();   $(this).text('Unmute').removeClass('btn-danger').addClass('btn-success'); }
		audiobridgeHandle.send({ message: { request: 'configure', muted: !muted } });
	});

	// Show/hide provider-specific config fields
	$('#sttProvider').on('change', function() {
		var isOpenai = $(this).val() === 'openai';
		$('#openaiConfig').toggleClass('d-none', !isOpenai);
		$('#awsConfig').toggleClass('d-none', isOpenai);
	});

	// Load ABMod
	$('#loadabmod').on('click', function() {
		if(!myid) return;
		var cfg = buildAbmodConfig();
		audiobridgeHandle.send({ message: {
			request: 'configure',
			abmod_load: ABMOD_SO_PATH,
			abmod_config: JSON.stringify(cfg)
		}});
		$(this).prop('disabled', true);
		$('#unloadabmod').prop('disabled', false);
		$('#sttStatus').text('loading\u2026');
	});

	// Unload ABMod
	$('#unloadabmod').on('click', function() {
		if(!myid) return;
		audiobridgeHandle.send({ message: { request: 'configure', abmod_unload: true } });
		$(this).prop('disabled', true);
		$('#loadabmod').prop('disabled', false);
		$('#sttStatus').text('idle');
	});
});
