// audiobridge-ui.js
// UI and main flow for Janus audiobridge demo, using AudiobridgeCore
import AudiobridgeCore from './audiobridge-core.js';

// Assume Janus, server, iceServers, and other globals are loaded from settings.js
let audiobridge = null;
let opaqueId = "audiobridgetest-" + Janus.randomString(12);
let remoteStream = null;
let myroom = 1234;
if(getQueryStringValue("room") !== "")
  myroom = parseInt(getQueryStringValue("room"));
let acodec = (getQueryStringValue("acodec") !== "" ? getQueryStringValue("acodec") : null);
let stereo = false;
if(getQueryStringValue("stereo") !== "")
  stereo = (getQueryStringValue("stereo") === "true");
let mygroup = null;
if(getQueryStringValue("group") !== "")
  mygroup = getQueryStringValue("group");
let audiosuspended = (getQueryStringValue("suspended") !== "") ? (getQueryStringValue("suspended") === "true") : false;

$(document).ready(function() {
  $('#start').one('click', function() {
    $(this).attr('disabled', true).unbind('click');
    if(!Janus.isWebrtcSupported()) {
      bootbox.alert("No WebRTC support... ");
      return;
    }
    audiobridge = new AudiobridgeCore({
      server,
      iceServers,
      room: myroom,
      acodec,
      stereo,
      group: mygroup,
      suspended: audiosuspended,
      onJoined: onJoined,
      onParticipants: onParticipants,
      onRoomChanged: onRoomChanged,
      onDestroyed: onDestroyed,
      onError: onError,
      onRemoteTrack: onRemoteTrack,
      onLocalTrack: onLocalTrack,
      onCleanup: onCleanup,
      onMute: onMute,
      onSuspend: onSuspend,
      onResume: onResume,
      onSpatialPosition: onSpatialPosition
    });
    audiobridge.initJanus(Janus, opaqueId).then(() => {
      $('#details').remove();
      $('#audiojoin').removeClass('hide');
      $('#registernow').removeClass('hide');
      $('#register').click(registerUsername);
      $('#username').focus();
      $('#start').removeAttr('disabled').html("Stop")
        .click(function() {
          $(this).attr('disabled', true);
          audiobridge.janus.destroy();
        });
    }).catch(onError);
  });
});

function onJoined(msg) {
  // Hide join UI, show room UI, set participant name
  $('#audiojoin').addClass('hide');
  $('#room').removeClass('hide');
  $('#participant').removeClass('hide').html(audiobridge.username).removeClass('hide');
  // Start WebRTC: create local offer to send mic
  startPeerConnection();
}

function onParticipants(list) {
  // Update the participants list UI
  $('#list').empty();
  list.forEach(function(p) {
    let id = p["id"];
    let display = escapeXmlTags(p["display"]);
    let setup = p["setup"];
    let muted = p["muted"];
    let suspended = p["suspended"];
    let spatial = p["spatial_position"];
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
  });
}

function onRoomChanged(msg) {
  // Clear and update participants list
  $('#list').empty();
  if(msg["participants"]) onParticipants(msg["participants"]);
}

function onDestroyed() {
  bootbox.alert("The room has been destroyed", function() {
    window.location.reload();
  });
}

function onError(error) {
  bootbox.alert(error, function() {
    window.location.reload();
  });
}

function onRemoteTrack(track, mid, on, metadata) {
  if(remoteStream || track.kind !== "audio")
    return;
  if(!on) {
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
  $('#toggleaudio').off('click').click(function() {
    let muted = $('#toggleaudio').html() === "Unmute";
    audiobridge.mute(!muted);
  }).removeClass('hide');
}

function onLocalTrack(track, on) {
  // Hide join UI, show room UI, set participant name
  $('#audiojoin').addClass('hide');
  $('#room').removeClass('hide');
  $('#participant').removeClass('hide').html(audiobridge.username).removeClass('hide');
}

function onCleanup() {
  remoteStream = null;
  $('#participant').empty().addClass('hide');
  $('#list').empty();
  $('#mixedaudio').empty();
  $('#room').addClass('hide');
}

function onMute(muted) {
  if(muted) {
    $('#toggleaudio').html("Unmute").removeClass("btn-danger").addClass("btn-success");
  } else {
    $('#toggleaudio').html("Mute").removeClass("btn-success").addClass("btn-danger");
  }
}

function onSuspend() {
  $('#togglesuspend').html("Resume").removeClass("btn-secondary").addClass("btn-info");
}

function onResume() {
  $('#togglesuspend').html("Suspend").removeClass("btn-info").addClass("btn-secondary");
}

function onSpatialPosition(position) {
  // Prompt for new spatial position
  bootbox.prompt("Insert new spatial position: [0-100] (0=left, 50=center, 100=right)", function(result) {
    let spatial = parseInt(result);
    if(isNaN(spatial) || spatial < 0 || spatial > 100) {
      bootbox.alert("Invalid value");
      return;
    }
    audiobridge.setSpatialPosition(spatial);
  });
}

function escapeXmlTags(value) {
  if(value) {
    let escapedValue = value.replace(new RegExp('<', 'g'), '&lt');
    escapedValue = escapedValue.replace(new RegExp('>', 'g'), '&gt');
    return escapedValue;
  }
}

function registerUsername() {
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
  audiobridge.join(username);
}

function getQueryStringValue(name) {
  name = name.replace(/[[]/, "\\[").replace(/[\]]/, "\\]");
  let regex = new RegExp("[\\?&]" + name + "=([^&#]*)"),
    results = regex.exec(location.search);
  return results === null ? "" : decodeURIComponent(results[1].replace(/\+/g, " "));
} 

// Create a local offer (audio only) and send to the plugin, to trigger mic permission and PC setup
function startPeerConnection() {
  if(!audiobridge || !audiobridge.pluginHandle)
    return;
  audiobridge.pluginHandle.createOffer({
    media: { audio: true, video: false, data: true },
    success: function(jsep) {
      let body = { request: 'configure', muted: false, audio: true };
      audiobridge.pluginHandle.send({ message: body, jsep: jsep });
    },
    error: function(error) {
      if(onError) onError(error);
    }
  });
}