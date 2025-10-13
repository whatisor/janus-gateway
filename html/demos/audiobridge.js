// We import the settings.js file to know which address we should contact
// to talk to Janus, and optionally which STUN/TURN servers should be
// used as well. Specifically, that file defines the "server" and
// "iceServers" properties we'll pass when creating the Janus session.

/* global iceServers:readonly, Janus:readonly, server:readonly */

var janus = null;
var mixertest = null;
var opaqueId = "audiobridgetest-"+Janus.randomString(12);

var remoteStream = null;

var myroom = 1234;	// Demo room
if(getQueryStringValue("room") !== "")
	myroom = parseInt(getQueryStringValue("room"));
var acodec = (getQueryStringValue("acodec") !== "" ? getQueryStringValue("acodec") : null);
var stereo = false;
if(getQueryStringValue("stereo") !== "")
	stereo = (getQueryStringValue("stereo") === "true");
var mygroup = null;	// Forwarding group, if required by the room
if(getQueryStringValue("group") !== "")
	mygroup = getQueryStringValue("group");
var myusername = null;
var myid = null;
var webrtcUp = false;
var audioenabled = false;
var audiosuspended = (getQueryStringValue("suspended") !== "") ? (getQueryStringValue("suspended") === "true") : false;

// STT conversation state
var sttKnownUsers = {}; // id -> display
var sttPartialItems = {}; // userKey -> <li> node for current partial
function sttRememberUser(id, display) {
  if(id !== undefined && id !== null) sttKnownUsers[String(id)] = display || ("User " + id);
  if(display) sttKnownUsers[String(display)] = display; // allow lookup by display string too
}
function sttResolveName(userField) {
  if(userField && sttKnownUsers[String(userField)]) return sttKnownUsers[String(userField)];
  if(userField && /[^0-9]/.test(String(userField))) return String(userField); // looks like a name already
  return null;
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
  li.innerHTML = (ts ? '<small class="text-muted">[' + escapeXmlTags(ts) + ']</small> ' : '') + '<strong>' + escapeXmlTags(who) + ':</strong> ' + escapeXmlTags(text);
  if(isFinal) li.className += ' list-group-item-light';
  const ul = document.getElementById('sttConversation');
  if(ul) {
    ul.appendChild(li);
    // Scroll the surrounding card-body instead of the UL itself
    const cont = ul.parentElement;
    if(cont && cont.scrollTo) cont.scrollTo({ top: cont.scrollHeight, behavior: 'auto' });
    else if(cont) cont.scrollTop = cont.scrollHeight;
  }
}

function sttUpdatePartial(userKey, displayName, text) {
  const ul = document.getElementById('sttConversation');
  if(!ul) return;
  if(!text || !text.trim()) return;
  let li = sttPartialItems[userKey];
  if(!li) {
    li = document.createElement('li');
    li.className = 'list-group-item';
    sttPartialItems[userKey] = li;
    ul.appendChild(li);
  }
  const ts = sttNowTime();
  li.innerHTML = (ts ? '<small class="text-muted">[' + escapeXmlTags(ts) + ']</small> ' : '') + '<strong>' + escapeXmlTags(displayName || userKey || 'Unknown') + ':</strong> ' + escapeXmlTags(text) + ' <em class="text-muted">(partial)</em>';
  const cont = ul.parentElement;
  if(cont && cont.scrollTo) cont.scrollTo({ top: cont.scrollHeight, behavior: 'auto' });
  else if(cont) cont.scrollTop = cont.scrollHeight;
}

function sttFinalize(userKey, displayName, text) {
  const ul = document.getElementById('sttConversation');
  if(!ul) return;
  const finalText = (text && text.trim()) ? text : (sttPartialItems[userKey] ? sttPartialItems[userKey].textContent.replace(/^.*?:\s*/, '') : '');
  // Remove partial row (we will append a new final row to keep full history)
  if(sttPartialItems[userKey]) {
    try { ul.removeChild(sttPartialItems[userKey]); } catch(e) {}
    delete sttPartialItems[userKey];
  }
  if(finalText) sttAppendMessage(displayName || userKey || 'Unknown', finalText, true);
}


$(document).ready(function() {
	// Initialize the library (all console debuggers enabled)
	Janus.init({debug: "all", callback: function() {
		// Use a button to start the demo
		$('#start').one('click', function() {
			$(this).attr('disabled', true).unbind('click');
			// Make sure the browser supports WebRTC
			if(!Janus.isWebrtcSupported()) {
				bootbox.alert("No WebRTC support... ");
				return;
			}
			// Create session
			janus = new Janus(
				{
					server: server,
					iceServers: iceServers,
					// Should the Janus API require authentication, you can specify either the API secret or user token here too
					//		token: "mytoken",
					//	or
					//		apisecret: "serversecret",
					success: function() {
						// Attach to AudioBridge plugin
						janus.attach(
							{
								plugin: "janus.plugin.audiobridge",
								opaqueId: opaqueId,
								success: function(pluginHandle) {
									$('#details').remove();
									mixertest = pluginHandle;
									Janus.log("Plugin attached! (" + mixertest.getPlugin() + ", id=" + mixertest.getId() + ")");
									// Prepare the username registration
									$('#audiojoin').removeClass('hide');
									$('#registernow').removeClass('hide');
									$('#register').click(registerUsername);
									$('#username').focus();
									$('#start').removeAttr('disabled').html("Stop")
										.click(function() {
											$(this).attr('disabled', true);
											janus.destroy();
										});
								},
								error: function(error) {
									Janus.error("  -- Error attaching plugin...", error);
									bootbox.alert("Error attaching plugin... " + error);
								},
								consentDialog: function(on) {
									Janus.debug("Consent dialog should be " + (on ? "on" : "off") + " now");
									if(on) {
										// Darken screen and show hint
										$.blockUI({
											message: '<div><img src="up_arrow.png"/></div>',
											baseZ: 3001,
											css: {
												border: 'none',
												padding: '15px',
												backgroundColor: 'transparent',
												color: '#aaa',
												top: '10px',
												left: '100px'
											} });
									} else {
										// Restore screen
										$.unblockUI();
									}
								},
								iceState: function(state) {
									Janus.log("ICE state changed to " + state);
								},
								mediaState: function(medium, on, mid) {
									Janus.log("Janus " + (on ? "started" : "stopped") + " receiving our " + medium + " (mid=" + mid + ")");
								},
								webrtcState: function(on) {
									Janus.log("Janus says our WebRTC PeerConnection is " + (on ? "up" : "down") + " now");
								},
								onmessage: function(msg, jsep) {
									Janus.debug(" ::: Got a message :::", msg);
									let event = msg["audiobridge"];
									Janus.debug("Event: " + event);
									if(event) {
										// Handle custom AudioBridge module (ABMod) notifications for STT
										if(event === "abmod") {
											// Expected payload shape:
											// { audiobridge: "abmod", room, event: "transcription.partial|final|error", payload: { user, text } }
											let sttType = msg["event"];
											let payload = msg["payload"] || {};
                                            let userField = payload.user || "";
                                            // Resolve display name: try id->display mapping, then direct name, then fallback
                                            let who = sttResolveName(userField) || myusername || "Speaker";
											// Update status
											$("#sttStatus").text(sttType === "transcription.final" ? "final" : "listening…");
                                            // Use stable key by participant id when available, else by resolved name
                                            const userKey = (payload.user && sttKnownUsers[String(payload.user)]) ? String(payload.user) : String(who);
											if(sttType === 'transcription.partial') {
												sttUpdatePartial(userKey, who, payload.text || '');
											} else if(sttType === 'transcription.final') {
												sttFinalize(userKey, who, payload.text || '');
											} else if(sttType === 'transcription.error') {
												if(payload.text) sttAppendMessage(who, payload.text, true);
											}
											return; // Don't process further as regular audiobridge events
										}
										if(event === "joined") {
											// Successfully joined, negotiate WebRTC now
											if(msg["id"]) {
												myid = msg["id"];
												Janus.log("Successfully joined room " + msg["room"] + " with ID " + myid);
												if(!webrtcUp) {
													webrtcUp = true;
													// Publish our stream
													mixertest.createOffer(
														{
															// We only want bidirectional audio
															tracks: [
																{ type: 'audio', capture: true, recv: true },
															],
															customizeSdp: function(jsep) {
																if(stereo && jsep.sdp.indexOf("stereo=1") == -1) {
																	// Make sure that our offer contains stereo too
																	jsep.sdp = jsep.sdp.replace("useinbandfec=1", "useinbandfec=1;stereo=1");
																	// Create a spinner waiting for the remote video
																	$('#mixedaudio').html(
																		'<div class="text-center">' +
																		'	<div id="spinner" class="spinner-border" role="status">' +
																		'		<span class="visually-hidden">Loading...</span>' +
																		'	</div>' +
																		'</div>');
																}
															},
															success: function(jsep) {
																Janus.debug("Got SDP!", jsep);
																let publish = { request: "configure", muted: false };
								mixertest.send({ message: publish, jsep: jsep });
								// Also expose buttons to load/unload the custom AB module
								if($('#loadabmod').length === 0) {
									$('.top-right').append('<button class="btn btn-warning" id="loadabmod">Load ABMod</button>');
									$('#loadabmod').off('click').on('click', function() {
										let path = '/opt/janus/lib/janus/abmodules/libabmod_transcriber_template.so';
										mixertest.send({ message: { request: 'configure', abmod_load: path, abmod_config: '{}' } });
									});
								}
								if($('#unloadabmod').length === 0) {
									$('.top-right').append('<button class="btn btn-outline-warning" id="unloadabmod">Unload ABMod</button>');
									$('#unloadabmod').off('click').on('click', function() {
										mixertest.send({ message: { request: 'configure', abmod_unload: true } });
									});
								}
															},
															error: function(error) {
																Janus.error("WebRTC error:", error);
																bootbox.alert("WebRTC error... " + error.message);
															}
														});
												}
											}
											// Any room participant?
											if(msg["participants"]) {
												let list = msg["participants"];
												Janus.debug("Got a list of participants:", list);
												for(let f in list) {
													let id = list[f]["id"];
													let display = escapeXmlTags(list[f]["display"]);
                                            sttRememberUser(id, display);
													let setup = list[f]["setup"];
													let muted = list[f]["muted"];
													let suspended = list[f]["suspended"];
													let spatial = list[f]["spatial_position"];
													Janus.debug("  >> [" + id + "] " + display + " (setup=" + setup + ", muted=" + muted + ")");
													if($('#rp' + id).length === 0) {
														// Add to the participants list
														let slider = '';
														if(spatial !== null && spatial !== undefined)
															slider = '<span>[L <input id="sp' + id + '" type="text" style="width: 10%;"/> R] </span>';
														$('#list').append('<li id="rp' + id +'" class="list-group-item">' +
															slider +
															display +
															' <i class="absetup fa-solid fa-link-slash" title="No PeerConnection"></i>' +
															' <i class="absusp fa-solid fa-eye-slash" title="Suspended"></i>' +
															' <i class="abmuted fa-solid fa-microphone-slash" title="Muted"></i></li>');
														if(spatial !== null && spatial !== undefined) {
															$('#sp' + id).slider({ min: 0, max: 100, step: 1, value: 50, handle: 'triangle', enabled: false });
															$('#position').removeClass('hide');
														}
														$('#rp' + id + ' > i').addClass('hide');
													}
													if(muted === true || muted === "true")
														$('#rp' + id + ' > i.abmuted').removeClass('hide');
													else
														$('#rp' + id + ' > i.abmuted').addClass('hide');
													if(setup === true || setup === "true")
														$('#rp' + id + ' > i.absetup').addClass('hide');
													else
														$('#rp' + id + ' > i.absetup').removeClass('hide');
													if(suspended === true)
														$('#rp' + id + ' > i.absusp').removeClass('hide');
													else
														$('#rp' + id + ' > i.absusp').addClass('hide');
													if(spatial !== null && spatial !== undefined)
														$('#sp' + id).slider('setValue', spatial);
												}
											}
										} else if(event === "roomchanged") {
											// The user switched to a different room
											myid = msg["id"];
											Janus.log("Moved to room " + msg["room"] + ", new ID: " + myid);
											// Any room participant?
											$('#list').empty();
											if(msg["participants"]) {
												let list = msg["participants"];
												Janus.debug("Got a list of participants:", list);
												for(let f in list) {
													let id = list[f]["id"];
													let display = escapeXmlTags(list[f]["display"]);
                                                sttRememberUser(id, display);
													let setup = list[f]["setup"];
													let muted = list[f]["muted"];
													let suspended = list[f]["suspended"];
													let spatial = list[f]["spatial_position"];
													Janus.debug("  >> [" + id + "] " + display + " (setup=" + setup + ", muted=" + muted + ")");
													if($('#rp' + id).length === 0) {
														// Add to the participants list
														let slider = '';
														if(spatial !== null && spatial !== undefined)
															slider = '<span>[L <input id="sp' + id + '" type="text" style="width: 10%;"/> R] </span>';
														$('#list').append('<li id="rp' + id +'" class="list-group-item">' +
															slider +
															display +
																' <i class="absetup fa-solid fa-link-slash" title="No PeerConnection"></i>' +
																' <i class="absusp fa-solid fa-eye-slash" title="Suspended"></i>' +
																' <i class="abmuted fa-solid fa-microphone-slash" title="Muted"></i></li>');
														if(spatial !== null && spatial !== undefined) {
															$('#sp' + id).slider({ min: 0, max: 100, step: 1, value: 50, handle: 'triangle', enabled: false });
															$('#position').removeClass('hide');
														}
														$('#rp' + id + ' > i').addClass('hide');
													}
													if(muted === true || muted === "true")
														$('#rp' + id + ' > i.abmuted').removeClass('hide');
													else
														$('#rp' + id + ' > i.abmuted').addClass('hide');
													if(setup === true || setup === "true")
														$('#rp' + id + ' > i.absetup').addClass('hide');
													else
														$('#rp' + id + ' > i.absetup').removeClass('hide');
													if(suspended === true)
														$('#rp' + id + ' > i.absusp').removeClass('hide');
													else
														$('#rp' + id + ' > i.absusp').addClass('hide');
													if(spatial !== null && spatial !== undefined)
														$('#sp' + id).slider('setValue', spatial);
												}
											}
										} else if(event === "destroyed") {
											// The room has been destroyed
											Janus.warn("The room has been destroyed!");
											bootbox.alert("The room has been destroyed", function() {
												window.location.reload();
											});
										} else if(event === "event") {
											if(msg["participants"]) {
												if(msg["resumed"]) {
													// This is a full recap after a suspend: clear the list of participants
													$('#list').empty();
												}
												let list = msg["participants"];
												Janus.debug("Got a list of participants:", list);
												for(let f in list) {
													let id = list[f]["id"];
													let display = escapeXmlTags(list[f]["display"]);
                                                    sttRememberUser(id, display);
													let setup = list[f]["setup"];
													let muted = list[f]["muted"];
													let suspended = list[f]["suspended"];
													let spatial = list[f]["spatial_position"];
													Janus.debug("  >> [" + id + "] " + display + " (setup=" + setup + ", muted=" + muted + ")");
													if($('#rp' + id).length === 0) {
														// Add to the participants list
														let slider = '';
														if(spatial !== null && spatial !== undefined)
															slider = '<span>[L <input id="sp' + id + '" type="text" style="width: 10%;"/> R] </span>';
														$('#list').append('<li id="rp' + id +'" class="list-group-item">' +
															slider +
															display +
																' <i class="absetup fa-solid fa-link-slash" title="No PeerConnection"></i>' +
																' <i class="absusp fa-solid fa-eye-slash" title="Suspended"></i>' +
																' <i class="abmuted fa-solid fa-microphone-slash" title="Muted"></i></li>');
														if(spatial !== null && spatial !== undefined) {
															$('#sp' + id).slider({ min: 0, max: 100, step: 1, value: 50, handle: 'triangle', enabled: false });
															$('#position').removeClass('hide');
														}
														$('#rp' + id + ' > i').addClass('hide');
													}
													if(muted === true || muted === "true")
														$('#rp' + id + ' > i.abmuted').removeClass('hide');
													else
														$('#rp' + id + ' > i.abmuted').addClass('hide');
													if(setup === true || setup === "true")
														$('#rp' + id + ' > i.absetup').addClass('hide');
													else
														$('#rp' + id + ' > i.absetup').removeClass('hide');
													if(suspended === true)
														$('#rp' + id + ' > i.absusp').removeClass('hide');
													else
														$('#rp' + id + ' > i.absusp').addClass('hide');
													if(spatial !== null && spatial !== undefined)
														$('#sp' + id).slider('setValue', spatial);
												}
											} else if(msg["suspended"]) {
												let id = msg["suspended"];
												$('#rp' + id + ' > i.absusp').removeClass('hide');
											} else if(msg["resumed"]) {
												let id = msg["resumed"];
												$('#rp' + id + ' > i.absusp').addClass('hide');
											} else if(msg["error"]) {
												if(msg["error_code"] === 485) {
													// This is a "no such room" error: give a more meaningful description
													bootbox.alert(
														"<p>Apparently room <code>" + myroom + "</code> (the one this demo uses as a test room) " +
														"does not exist...</p><p>Do you have an updated <code>janus.plugin.audiobridge.jcfg</code> " +
														"configuration file? If not, make sure you copy the details of room <code>" + myroom + "</code> " +
														"from that sample in your current configuration file, then restart Janus and try again."
													);
												} else {
													bootbox.alert(msg["error"]);
												}
												return;
											}
											// Any new feed to attach to?
											if(msg["leaving"]) {
												// One of the participants has gone away?
												let leaving = msg["leaving"];
												Janus.log("Participant left: " + leaving + " (we have " + $('#rp'+leaving).length + " elements with ID #rp" +leaving + ")");
												$('#rp'+leaving).remove();
											}
										}
									}
									if(jsep) {
										Janus.debug("Handling SDP as well...", jsep);
										mixertest.handleRemoteJsep({ jsep: jsep });
									}
								},
								onlocaltrack: function(track, on) {
									Janus.debug("Local track " + (on ? "added" : "removed") + ":", track);
									// We're not going to attach the local audio stream
									$('#audiojoin').addClass('hide');
									$('#room').removeClass('hide');
									$('#participant').removeClass('hide').html(myusername).removeClass('hide');
								},
								onremotetrack: function(track, mid, on, metadata) {
									Janus.debug(
										"Remote track (mid=" + mid + ") " +
										(on ? "added" : "removed") +
										(metadata ? " (" + metadata.reason + ") " : "") + ":", track
									);
									if(remoteStream || track.kind !== "audio")
										return;
									if(!on) {
										// Track removed, get rid of the stream and the rendering
										remoteStream = null;
										$('#roomaudio').remove();
										return;
									}
									$('#spinner').remove();
									remoteStream = new MediaStream([track]);
									$('#room').removeClass('hide');
									if($('#roomaudio').length === 0) {
										$('#mixedaudio').append('<audio class="rounded centered w-100" id="roomaudio" controls autoplay/>');
										$('#roomaudio').get(0).volume = 0;
									}
									Janus.attachMediaStream($('#roomaudio').get(0), remoteStream);
									$('#roomaudio').get(0).play();
									$('#roomaudio').get(0).volume = 1;
									// Mute button
									audioenabled = true;
									$('#toggleaudio').click(
										function() {
											audioenabled = !audioenabled;
											if(audioenabled)
												$('#toggleaudio').html("Mute").removeClass("btn-success").addClass("btn-danger");
											else
												$('#toggleaudio').html("Unmute").removeClass("btn-danger").addClass("btn-success");
											mixertest.send({ message: { request: "configure", muted: !audioenabled }});
										}).removeClass('hide');
									// Suspend button
									if(!audiosuspended)
										$('#togglesuspend').html("Suspend").removeClass("btn-info").addClass("btn-secondary");
									else
										$('#togglesuspend').html("Resume").removeClass("btn-secondary").addClass("btn-info");
									$('#togglesuspend').click(
										function() {
											audiosuspended = !audiosuspended;
											if(!audiosuspended)
												$('#togglesuspend').html("Suspend").removeClass("btn-info").addClass("btn-secondary");
											else
												$('#togglesuspend').html("Resume").removeClass("btn-secondary").addClass("btn-info");
											mixertest.send({ message: {
												request: (audiosuspended ? "suspend" : "resume"),
												room: myroom,
												id: myid
											}});
										}).removeClass('hide');
									// Spatial position, if enabled
									$('#position').click(
										function() {
											bootbox.prompt("Insert new spatial position: [0-100] (0=left, 50=center, 100=right)", function(result) {
												let spatial = parseInt(result);
												if(isNaN(spatial) || spatial < 0 || spatial > 100) {
													bootbox.alert("Invalid value");
													return;
												}
												mixertest.send({ message: { request: "configure", spatial_position: spatial }});
											});
										});
								},
								oncleanup: function() {
									webrtcUp = false;
									Janus.log(" ::: Got a cleanup notification :::");
									$('#participant').empty().addClass('hide');
									$('#list').empty();
									$('#mixedaudio').empty();
									$('#room').addClass('hide');
									remoteStream = null;
								}
							});
					},
					error: function(error) {
						Janus.error(error);
						bootbox.alert(error, function() {
							window.location.reload();
						});
					},
					destroyed: function() {
						window.location.reload();
					}
				});
		});
	}});
});

// eslint-disable-next-line no-unused-vars
function checkEnter(field, event) {
	let theCode = event.keyCode ? event.keyCode : event.which ? event.which : event.charCode;
	if(theCode == 13) {
		registerUsername();
		return false;
	} else {
		return true;
	}
}

function registerUsername() {
	if($('#username').length === 0) {
		// Create fields to register
		$('#register').click(registerUsername);
		$('#username').focus();
	} else {
		// Try a registration
		$('#username').attr('disabled', true);
		$('#register').attr('disabled', true).unbind('click');
		let username = $('#username').val();
		if(username === "") {
			$('#you')
				.removeClass().addClass('badge bg-warning')
				.html("Insert your display name (e.g., pippo)");
			$('#username').removeAttr('disabled');
			$('#register').removeAttr('disabled').click(registerUsername);
			return;
		}
		if(/[^a-zA-Z0-9]/.test(username)) {
			$('#you')
				.removeClass().addClass('badge bg-warning')
				.html('Input is not alphanumeric');
			$('#username').removeAttr('disabled').val("");
			$('#register').removeAttr('disabled').click(registerUsername);
			return;
		}
		let register = { request: "join", room: myroom, display: username, suspended: audiosuspended };
		myusername = escapeXmlTags(username);
		// Check if we need to join using G.711 instead of (default) Opus
		if(acodec === 'opus' || acodec === 'pcmu' || acodec === 'pcma')
			register.codec = acodec;
		// If the room uses forwarding groups, this is how we state ours
		if(mygroup)
			register["group"] = mygroup;
		// Send the message
		mixertest.send({ message: register });
	}
}

// Helper to parse query string
function getQueryStringValue(name) {
	name = name.replace(/[[]/, "\\[").replace(/[\]]/, "\\]");
	let regex = new RegExp("[\\?&]" + name + "=([^&#]*)"),
		results = regex.exec(location.search);
	return results === null ? "" : decodeURIComponent(results[1].replace(/\+/g, " "));
}

// Helper to escape XML tags
function escapeXmlTags(value) {
	if(value) {
		let escapedValue = value.replace(new RegExp('<', 'g'), '&lt');
		escapedValue = escapedValue.replace(new RegExp('>', 'g'), '&gt');
		return escapedValue;
	}
}
