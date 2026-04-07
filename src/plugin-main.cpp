/*
 * Deepgram Captions for OBS
 * Real-time speech-to-text captions using Deepgram Nova-3 API
 *
 * Audio capture → Deepgram WebSocket → Real-time caption display
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── Source data structure ───
struct deepgram_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;

	// Hotkey
	obs_hotkey_id hotkey_id{OBS_INVALID_HOTKEY_ID};

	// WebSocket
	std::unique_ptr<ix::WebSocket> websocket;
	std::atomic<bool> connected{false};
	std::atomic<bool> captioning{false};
	std::atomic<bool> stopping{false};

	// KeepAlive thread
	std::thread keepalive_thread;
	std::atomic<bool> keepalive_running{false};

	// Audio capture
	obs_source_t *audio_source{nullptr};
	std::string audio_source_name;

	// Caption state
	std::mutex text_mutex;
	std::string final_buffer;
	std::string partial_text;
	int utterance_count{0};

	// Settings
	int font_size{48};
	std::string font_face{"Apple SD Gothic Neo"};
	std::string api_key;
	std::string language{"ko"};
	std::string model{"nova-3"};
	bool smart_format{true};
	bool punctuate{true};
	bool interim_results{true};
	int endpointing_ms{300};
};

// ─── Update text display ───
static void update_text_display(deepgram_caption_data *data, const char *text)
{
	if (!data->text_source)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "flags", 0);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);
	obs_source_update(data->text_source, s);

	obs_data_release(font);
	obs_data_release(s);
}

// ─── Audio capture callback ───
static void audio_capture_callback(void *param, obs_source_t *, const struct audio_data *audio,
				   bool muted)
{
	auto *data = static_cast<deepgram_caption_data *>(param);

	if (!data->captioning || !data->connected || !data->websocket || muted)
		return;
	if (!audio->data[0] || audio->frames == 0)
		return;

	// OBS: float32, 48000Hz → Deepgram: pcm_s16le, 16000Hz (3:1 downsample)
	const float *src = reinterpret_cast<const float *>(audio->data[0]);
	uint32_t src_frames = audio->frames;
	uint32_t dst_frames = src_frames / 3;
	if (dst_frames == 0)
		return;

	std::vector<int16_t> pcm16(dst_frames);
	for (uint32_t i = 0; i < dst_frames; i++) {
		float sample = src[i * 3];
		if (sample > 1.0f)
			sample = 1.0f;
		if (sample < -1.0f)
			sample = -1.0f;
		pcm16[i] = static_cast<int16_t>(sample * 32767.0f);
	}

	data->websocket->sendBinary(
		std::string(reinterpret_cast<const char *>(pcm16.data()), pcm16.size() * sizeof(int16_t)));
}

// ─── Handle Deepgram response message ───
static void handle_deepgram_message(deepgram_caption_data *data, const std::string &msg_str)
{
	try {
		json resp = json::parse(msg_str);

		std::string msg_type = resp.value("type", "");

		// Metadata message at connection start
		if (msg_type == "Metadata") {
			obs_log(LOG_INFO, "Deepgram session started (request_id: %s)",
				resp.value("request_id", "").c_str());
			return;
		}

		// UtteranceEnd: speaker finished speaking
		if (msg_type == "UtteranceEnd") {
			std::lock_guard<std::mutex> lock(data->text_mutex);
			std::string line = data->final_buffer;
			while (!line.empty() && line.front() == ' ')
				line.erase(line.begin());
			while (!line.empty() && line.back() == ' ')
				line.pop_back();

			if (!line.empty()) {
				data->utterance_count++;
				obs_log(LOG_INFO, "[Utterance %d] %s", data->utterance_count, line.c_str());
			}

			data->final_buffer.clear();
			data->partial_text.clear();
			update_text_display(data, "");
			return;
		}

		// SpeechStarted event
		if (msg_type == "SpeechStarted") {
			return;
		}

		// Error response
		if (msg_type == "Error" || resp.contains("err_code")) {
			std::string err = resp.value("err_msg", resp.value("message", "Unknown error"));
			obs_log(LOG_ERROR, "Deepgram error: %s", err.c_str());
			update_text_display(data, ("Error: " + err).c_str());
			return;
		}

		// Main transcription results
		if (msg_type != "Results")
			return;

		auto channel = resp.value("channel", json::object());
		auto alternatives = channel.value("alternatives", json::array());
		if (alternatives.empty())
			return;

		std::string transcript = alternatives[0].value("transcript", "");
		bool is_final = resp.value("is_final", false);
		bool speech_final = resp.value("speech_final", false);

		std::lock_guard<std::mutex> lock(data->text_mutex);

		if (is_final && !transcript.empty()) {
			// Final result: append to buffer
			if (!data->final_buffer.empty())
				data->final_buffer += " ";
			data->final_buffer += transcript;

			if (speech_final) {
				// Speaker finished: log and clear
				std::string line = data->final_buffer;
				while (!line.empty() && line.front() == ' ')
					line.erase(line.begin());
				while (!line.empty() && line.back() == ' ')
					line.pop_back();

				if (!line.empty()) {
					data->utterance_count++;
					obs_log(LOG_INFO, "[Utterance %d] %s", data->utterance_count,
						line.c_str());
				}

				// Display final result briefly, then clear
				update_text_display(data, data->final_buffer.c_str());
				data->final_buffer.clear();
				data->partial_text.clear();
			} else {
				// Final but not speech_final: show accumulated text
				update_text_display(data, data->final_buffer.c_str());
			}
		} else if (!is_final && !transcript.empty()) {
			// Interim result: show current buffer + partial
			std::string display = data->final_buffer;
			if (!display.empty())
				display += " ";
			display += transcript;

			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());

			data->partial_text = transcript;
			update_text_display(data, display.c_str());
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "JSON parse error: %s", e.what());
	}
}

// ─── KeepAlive thread ───
static void keepalive_thread_func(deepgram_caption_data *data)
{
	while (data->keepalive_running) {
		// Sleep 5 seconds in 100ms increments for responsive shutdown
		for (int i = 0; i < 50 && data->keepalive_running; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (data->keepalive_running && data->connected && data->websocket) {
			data->websocket->send(R"({"type":"KeepAlive"})");
		}
	}
}

// ─── Stop captioning ───
static void stop_captioning(deepgram_caption_data *data)
{
	if (!data->captioning)
		return;

	data->stopping = true;
	data->captioning = false;
	data->connected = false;

	// Stop keepalive thread
	data->keepalive_running = false;
	if (data->keepalive_thread.joinable())
		data->keepalive_thread.join();

	if (data->audio_source) {
		obs_source_remove_audio_capture_callback(data->audio_source, audio_capture_callback,
							 data);
		obs_source_release(data->audio_source);
		data->audio_source = nullptr;
	}

	if (data->websocket) {
		// Send CloseStream before stopping
		data->websocket->send(R"({"type":"CloseStream"})");
		data->websocket->stop();
		data->websocket.reset();
	}

	data->stopping = false;
	update_text_display(data, "Deepgram Captions Ready!");
	obs_log(LOG_INFO, "Captioning stopped");
}

// ─── Build Deepgram WebSocket URL with query parameters ───
static std::string build_deepgram_url(deepgram_caption_data *data)
{
	std::string url = "wss://api.deepgram.com/v1/listen";
	url += "?model=" + data->model;
	url += "&language=" + data->language;
	url += "&encoding=linear16";
	url += "&sample_rate=16000";
	url += "&channels=1";
	url += "&endpointing=" + std::to_string(data->endpointing_ms);

	if (data->punctuate)
		url += "&punctuate=true";
	if (data->smart_format)
		url += "&smart_format=true";
	if (data->interim_results)
		url += "&interim_results=true";

	// Enable utterance end detection for clearing captions
	url += "&utterance_end_ms=1500";
	url += "&vad_events=true";

	return url;
}

// ─── Start captioning ───
static void start_captioning(deepgram_caption_data *data)
{
	if (data->captioning)
		stop_captioning(data);

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	obs_source_t *audio_src = obs_get_source_by_name(data->audio_source_name.c_str());
	if (!audio_src) {
		update_text_display(data, "[Select Audio Source!]");
		return;
	}

	update_text_display(data, "Connecting...");

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->final_buffer.clear();
		data->partial_text.clear();
		data->utterance_count = 0;
	}

	// 1. Setup WebSocket
	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl(build_deepgram_url(data));

	// Set authorization header
	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Token " + data->api_key;
	data->websocket->setExtraHeaders(headers);

	// Auto-reconnect: retry after 3s on disconnect
	data->websocket->enableAutomaticReconnection();
	data->websocket->setMinWaitBetweenReconnectionRetries(3000);
	data->websocket->setMaxWaitBetweenReconnectionRetries(30000);

	data->websocket->setOnMessageCallback([data](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			obs_log(LOG_INFO, "Deepgram WebSocket connected");
			data->connected = true;
			update_text_display(data, "Listening...");
			break;
		}

		case ix::WebSocketMessageType::Message:
			handle_deepgram_message(data, msg->str);
			break;

		case ix::WebSocketMessageType::Error:
			obs_log(LOG_ERROR, "WS error: %s (status=%d)", msg->errorInfo.reason.c_str(),
				msg->errorInfo.http_status);
			data->connected = false;
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;

		case ix::WebSocketMessageType::Close:
			obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)", msg->closeInfo.code,
				msg->closeInfo.reason.c_str());
			data->connected = false;
			if (!data->stopping && data->captioning) {
				std::string reason = msg->closeInfo.reason.empty()
							    ? "Connection closed"
							    : msg->closeInfo.reason;
				update_text_display(data, reason.c_str());
			}
			break;

		default:
			break;
		}
	});

	// 2. Register audio capture (connected=false so no data sent yet)
	data->audio_source = audio_src;
	obs_source_add_audio_capture_callback(audio_src, audio_capture_callback, data);

	// 3. Enable captioning and start WebSocket
	data->captioning = true;
	data->websocket->start();

	// 4. Start KeepAlive thread (sends every 5 seconds)
	data->keepalive_running = true;
	data->keepalive_thread = std::thread(keepalive_thread_func, data);

	obs_log(LOG_INFO, "Caption started, waiting for connection...");
}

// ─── Test connection (no audio) ───
static void test_connection(deepgram_caption_data *data)
{
	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
	}

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	update_text_display(data, "Testing connection...");

	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl(build_deepgram_url(data));

	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Token " + data->api_key;
	data->websocket->setExtraHeaders(headers);
	data->websocket->disableAutomaticReconnection();

	data->websocket->setOnMessageCallback([data](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			data->connected = true;
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");
			break;
		}
		case ix::WebSocketMessageType::Message: {
			try {
				json resp = json::parse(msg->str);
				if (resp.contains("err_code"))
					update_text_display(
						data,
						("Error: " + resp.value("err_msg", "Unknown")).c_str());
				else if (resp.value("type", "") == "Metadata")
					update_text_display(data, "Connected! Ready.");
			} catch (...) {
			}
			break;
		}
		case ix::WebSocketMessageType::Error:
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;
		case ix::WebSocketMessageType::Close:
			if (!data->stopping)
				update_text_display(data, "Test: Disconnected");
			data->connected = false;
			break;
		default:
			break;
		}
	});

	data->websocket->start();
}

// ─── Hotkey: Toggle Start/Stop Caption ───
static void hotkey_toggle_caption(void *private_data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *data = static_cast<deepgram_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->model = obs_data_get_string(settings, "model");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->smart_format = obs_data_get_bool(settings, "smart_format");
	data->punctuate = obs_data_get_bool(settings, "punctuate");
	data->interim_results = obs_data_get_bool(settings, "interim_results");
	data->endpointing_ms = (int)obs_data_get_int(settings, "endpointing_ms");
	obs_data_release(settings);

	if (data->captioning)
		stop_captioning(data);
	else
		start_captioning(data);
}

// ─── Callback functions ───

static const char *deepgram_caption_get_name(void *)
{
	return "Deepgram Captions";
}

static void *deepgram_caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new deepgram_caption_data();
	data->source = source;
	data->font_size = (int)obs_data_get_int(settings, "font_size");

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Deepgram Captions Ready!");
	obs_data_set_int(ts, "font_size", data->font_size);
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "deepgram_text", ts);
#else
	data->text_source = obs_source_create_private("text_ft2_source_v2", "deepgram_text", ts);
#endif
	obs_data_release(ts);

	data->hotkey_id = obs_hotkey_register_source(source, "deepgram_toggle_caption",
						     "Toggle Deepgram Captions", hotkey_toggle_caption,
						     data);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void deepgram_caption_destroy(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	stop_captioning(data);
	if (data->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(data->hotkey_id);
	if (data->text_source)
		obs_source_release(data->text_source);
	delete data;
}

static void deepgram_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	data->font_size = (int)obs_data_get_int(settings, "font_size");
	data->font_face = obs_data_get_string(settings, "font_face");
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->model = obs_data_get_string(settings, "model");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->smart_format = obs_data_get_bool(settings, "smart_format");
	data->punctuate = obs_data_get_bool(settings, "punctuate");
	data->interim_results = obs_data_get_bool(settings, "interim_results");
	data->endpointing_ms = (int)obs_data_get_int(settings, "endpointing_ms");

	if (!data->captioning && !data->connected) {
		if (!data->api_key.empty())
			update_text_display(data, "Deepgram Captions Ready!");
		else
			update_text_display(data, "[Set API Key in Properties]");
	}
}

// ─── Button callbacks ───
static bool on_test_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->model = obs_data_get_string(settings, "model");
	obs_data_release(settings);

	if (data->connected) {
		data->stopping = true;
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
		data->stopping = false;
		update_text_display(data, "Deepgram Captions Ready!");
	} else {
		test_connection(data);
	}
	return false;
}

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->model = obs_data_get_string(settings, "model");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->smart_format = obs_data_get_bool(settings, "smart_format");
	data->punctuate = obs_data_get_bool(settings, "punctuate");
	data->interim_results = obs_data_get_bool(settings, "interim_results");
	data->endpointing_ms = (int)obs_data_get_int(settings, "endpointing_ms");
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
		obs_property_set_description(property, "Start Caption");
	} else {
		start_captioning(data);
		obs_property_set_description(property, "Stop Caption");
	}
	return true;
}

// ─── Enumerate audio sources ───
static bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && strlen(name) > 0)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

// Properties UI
static obs_properties_t *deepgram_caption_get_properties(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	obs_properties_t *props = obs_properties_create();

	// API Key
	obs_properties_add_text(props, "api_key", "Deepgram API Key", OBS_TEXT_PASSWORD);

	// Audio source selection
	obs_property_t *audio_list =
		obs_properties_add_list(props, "audio_source", "Audio Source", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);

	// Model selection
	obs_property_t *model_list =
		obs_properties_add_list(props, "model", "Model", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model_list, "Nova-3 (Latest)", "nova-3");
	obs_property_list_add_string(model_list, "Nova-3 General", "nova-3-general");
	obs_property_list_add_string(model_list, "Nova-3 Medical", "nova-3-medical");
	obs_property_list_add_string(model_list, "Nova-2", "nova-2");
	obs_property_list_add_string(model_list, "Nova-2 General", "nova-2-general");

	// Language selection
	obs_property_t *lang =
		obs_properties_add_list(props, "language", "Language", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Korean", "ko");
	obs_property_list_add_string(lang, "English (US)", "en-US");
	obs_property_list_add_string(lang, "English (UK)", "en-GB");
	obs_property_list_add_string(lang, "Japanese", "ja");
	obs_property_list_add_string(lang, "Chinese (Simplified)", "zh");
	obs_property_list_add_string(lang, "Chinese (Traditional)", "zh-TW");
	obs_property_list_add_string(lang, "Spanish", "es");
	obs_property_list_add_string(lang, "French", "fr");
	obs_property_list_add_string(lang, "German", "de");
	obs_property_list_add_string(lang, "Portuguese", "pt");
	obs_property_list_add_string(lang, "Italian", "it");
	obs_property_list_add_string(lang, "Dutch", "nl");
	obs_property_list_add_string(lang, "Russian", "ru");
	obs_property_list_add_string(lang, "Hindi", "hi");
	obs_property_list_add_string(lang, "Multilingual (Auto)", "multi");

	// Transcription options
	obs_properties_add_bool(props, "smart_format", "Smart Format");
	obs_properties_add_bool(props, "punctuate", "Punctuation");
	obs_properties_add_bool(props, "interim_results", "Show Interim Results");

	// Endpointing sensitivity
	obs_properties_add_int_slider(props, "endpointing_ms", "Endpointing (ms)", 100, 1000, 50);

	// Font selection
	obs_property_t *font_list =
		obs_properties_add_list(props, "font_face", "Font", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
#ifdef _WIN32
	obs_property_list_add_string(font_list, "Malgun Gothic", "Malgun Gothic");
	obs_property_list_add_string(font_list, "Yu Gothic", "Yu Gothic");
#else
	obs_property_list_add_string(font_list, "Apple SD Gothic Neo", "Apple SD Gothic Neo");
	obs_property_list_add_string(font_list, "Hiragino Sans", "Hiragino Sans");
#endif
	obs_property_list_add_string(font_list, "Noto Sans CJK KR", "Noto Sans CJK KR");
	obs_property_list_add_string(font_list, "Noto Sans CJK JP", "Noto Sans CJK JP");
	obs_property_list_add_string(font_list, "Arial", "Arial");
	obs_property_list_add_string(font_list, "Helvetica", "Helvetica");

	// Font size
	obs_properties_add_int_slider(props, "font_size", "Font Size", 12, 120, 2);

	// Buttons
	obs_properties_add_button(props, "test_connection", "Test Connection", on_test_clicked);

	const char *btn_text = (data && data->captioning) ? "Stop Caption" : "Start Caption";
	obs_properties_add_button(props, "start_stop", btn_text, on_start_stop_clicked);

	return props;
}

static void deepgram_caption_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "api_key", "");
	obs_data_set_default_string(settings, "language", "ko");
	obs_data_set_default_string(settings, "model", "nova-3");
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_bool(settings, "smart_format", true);
	obs_data_set_default_bool(settings, "punctuate", true);
	obs_data_set_default_bool(settings, "interim_results", true);
	obs_data_set_default_int(settings, "endpointing_ms", 300);
#ifdef _WIN32
	obs_data_set_default_string(settings, "font_face", "Malgun Gothic");
#else
	obs_data_set_default_string(settings, "font_face", "Apple SD Gothic Neo");
#endif
	obs_data_set_default_int(settings, "font_size", 48);
}

static uint32_t deepgram_caption_get_width(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	return data->text_source ? obs_source_get_width(data->text_source) : 0;
}

static uint32_t deepgram_caption_get_height(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	return data->text_source ? obs_source_get_height(data->text_source) : 0;
}

static void deepgram_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
}

// ─── Source registration ───
static obs_source_info deepgram_caption_source_info = {};

bool obs_module_load(void)
{
	deepgram_caption_source_info.id = "deepgram_caption";
	deepgram_caption_source_info.type = OBS_SOURCE_TYPE_INPUT;
	deepgram_caption_source_info.output_flags = OBS_SOURCE_VIDEO;
	deepgram_caption_source_info.get_name = deepgram_caption_get_name;
	deepgram_caption_source_info.create = deepgram_caption_create;
	deepgram_caption_source_info.destroy = deepgram_caption_destroy;
	deepgram_caption_source_info.update = deepgram_caption_update;
	deepgram_caption_source_info.get_properties = deepgram_caption_get_properties;
	deepgram_caption_source_info.get_defaults = deepgram_caption_get_defaults;
	deepgram_caption_source_info.get_width = deepgram_caption_get_width;
	deepgram_caption_source_info.get_height = deepgram_caption_get_height;
	deepgram_caption_source_info.video_render = deepgram_caption_video_render;

	obs_register_source(&deepgram_caption_source_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
