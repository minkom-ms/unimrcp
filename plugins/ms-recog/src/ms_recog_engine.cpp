/*
 *
 * Copyright Microsoft
 *
 */

/*
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "apt_consumer_task.h"
#include "apt_log.h"
#include "mpf_activity_detector.h"
#include "mrcp_recog_engine.h"

#include "config_manager.h"
#include <cassert>
#include <common.h>
#include <condition_variable>
#include <functional>
#include <locale>
#include <memory>
#include <speechapi_cxx.h>
#include <speechapi_cxx_dialog_service_config.h>
#include <string>
#include <iostream>
#include <rapidjson/document.h>

using namespace Microsoft::CognitiveServices::Speech;
using namespace Microsoft::CognitiveServices::Speech::Dialog;
using namespace Microsoft::speechlib;

#define RECOG_ENGINE_TASK_NAME "MS Recog Engine"

typedef struct ms_recog_engine_t ms_recog_engine_t;
typedef struct ms_recog_channel_t ms_recog_channel_t;
typedef struct ms_recog_msg_t ms_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t ms_recog_engine_destroy(mrcp_engine_t* engine);
static apt_bool_t ms_recog_engine_open(mrcp_engine_t* engine);
static apt_bool_t ms_recog_engine_close(mrcp_engine_t* engine);
static mrcp_engine_channel_t*
ms_recog_engine_channel_create(mrcp_engine_t* engine, apr_pool_t* pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
    ms_recog_engine_destroy, ms_recog_engine_open, ms_recog_engine_close, ms_recog_engine_channel_create
};

/** Declaration of recognizer channel methods */
static apt_bool_t ms_recog_channel_destroy(mrcp_engine_channel_t* channel);
static apt_bool_t ms_recog_channel_open(mrcp_engine_channel_t* channel);
static apt_bool_t ms_recog_channel_close(mrcp_engine_channel_t* channel);
static apt_bool_t ms_recog_channel_request_process(mrcp_engine_channel_t* channel,
                                                   mrcp_message_t* request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
    ms_recog_channel_destroy, ms_recog_channel_open, ms_recog_channel_close, ms_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t ms_recog_stream_destroy(mpf_audio_stream_t* stream);
static apt_bool_t ms_recog_stream_open(mpf_audio_stream_t* stream, mpf_codec_t* codec);
static apt_bool_t ms_recog_stream_close(mpf_audio_stream_t* stream);
static apt_bool_t ms_recog_stream_write(mpf_audio_stream_t* stream, const mpf_frame_t* frame);

static apt_bool_t ms_recog_recognition_complete(ms_recog_channel_t* recog_channel,
                                                mrcp_recog_completion_cause_e cause);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
    ms_recog_stream_destroy, nullptr, nullptr, nullptr, ms_recog_stream_open, ms_recog_stream_close, ms_recog_stream_write, nullptr
};

/** Declaration of helper methods */
static std::string ms_recog_get_lexical_text_from_json_result(std::string& jsonResult);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source
 * priority. <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN, "RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Declaration of ms recognizer engine */
struct ms_recog_engine_t
{
    apt_consumer_task_t* task;
};

// static std::shared_ptr<SpeechRecognizer> createSpeechRecognizer();

struct RecogResource
{
    std::string result;
    std::string grammarId;
    std::shared_ptr<DialogServiceConfig> config;
    std::shared_ptr<Audio::PushAudioInputStream> pushStream;
    std::shared_ptr<DialogServiceConnector> recognizer;

    RecogResource()
    {
        // https://docs.microsoft.com/en-us/azure/cognitive-services/speech-service/speech-container-howto#speech-to-text-2

        // get speech SDK subscription key and region from config.json
        static auto subscriptionKey =
        ConfigManager::GetStrValue(Common::SPEECH_SECTION, Common::SPEECH_SDK_KEY);
        static auto region =
        ConfigManager::GetStrValue(Common::SPEECH_SECTION, Common::SPEECH_SDK_REGION);

        config = BotFrameworkConfig::FromSubscription(subscriptionKey, "");

        // we need endpoint override
        std::stringstream endpoint;
        endpoint << "wss://" << region;
        endpoint << ".convai.speech.microsoft.com/mrcp/api/v1";
        config->SetProperty("SPEECH-Endpoint", endpoint.str());
        config->SetProperty(PropertyId::Speech_LogFilename, "/tmp/speechsdk.log");

        // client ID. Unique identifier for the specific client
        // Very helpful to provide when reporting issues
        config->SetServiceProperty("clientId", "1182a496-d94f-4132-96e3-71933054705f", ServicePropertyChannel::UriQueryParameter);

        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Microsoft dialog recognition engine connection to %s", endpoint.str().c_str());

        // Request detailed output format.
        // config->SetOutputFormat(OutputFormat::Detailed);
    }
};

/** Declaration of ms recognizer channel */
struct ms_recog_channel_t
{
    /** Back pointer to engine */
    ms_recog_engine_t* ms_engine;
    /** Engine channel base */
    mrcp_engine_channel_t* channel;

    /** Active (in-progress) recognition request */
    mrcp_message_t* recog_request;
    /** Pending stop response */
    mrcp_message_t* stop_response;
    /** Indicates whether input timers are started */
    apt_bool_t timers_started;
    /** Voice activity detector */
    mpf_activity_detector_t* detector;
    /** File to write utterance to */
    FILE* audio_out;

    RecogResource* resource;
};

typedef enum
{
    MS_RECOG_MSG_OPEN_CHANNEL,
    MS_RECOG_MSG_CLOSE_CHANNEL,
    MS_RECOG_MSG_REQUEST_PROCESS
} ms_recog_msg_type_e;

/** Declaration of ms recognizer task message */
struct ms_recog_msg_t
{
    ms_recog_msg_type_e type;
    mrcp_engine_channel_t* channel;
    mrcp_message_t* request;
};

static apt_bool_t ms_recog_msg_signal(ms_recog_msg_type_e type,
                                      mrcp_engine_channel_t* channel,
                                      mrcp_message_t* request);
static apt_bool_t ms_recog_msg_process(apt_task_t* task, apt_task_msg_t* msg);

/** Create ms recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t* pool)
{
    auto* ms_engine =
    static_cast<ms_recog_engine_t*>(apr_palloc(pool, sizeof(ms_recog_engine_t)));

    apt_task_msg_pool_t* msg_pool =
    apt_task_msg_pool_create_dynamic(sizeof(ms_recog_msg_t), pool);
    ms_engine->task = apt_consumer_task_create(ms_engine, msg_pool, pool);
    if(!ms_engine->task)
    {
        return nullptr;
    }
    apt_task_t* task = apt_consumer_task_base_get(ms_engine->task);
    apt_task_name_set(task, RECOG_ENGINE_TASK_NAME);
    apt_task_vtable_t* vtable = apt_task_vtable_get(task);
    if(vtable)
    {
        vtable->process_msg = ms_recog_msg_process;
    }

    /* create engine base */
    return mrcp_engine_create(MRCP_RECOGNIZER_RESOURCE, /* MRCP resource identifier */
                              ms_engine,      /* object to associate */
                              &engine_vtable, /* virtual methods table of engine */
                              pool);          /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t ms_recog_engine_destroy(mrcp_engine_t* engine)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Microsoft recognition engine destroyed");

    ms_recog_engine_t* ms_engine = static_cast<ms_recog_engine_t*>(engine->obj);
    if(ms_engine->task)
    {
        apt_task_t* task = apt_consumer_task_base_get(ms_engine->task);
        apt_task_destroy(task);
        ms_engine->task = nullptr;
    }
    return true;
}

/** Open recognizer engine */
static apt_bool_t ms_recog_engine_open(mrcp_engine_t* engine)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Microsoft Recognition engine started");

    auto* ms_engine = static_cast<ms_recog_engine_t*>(engine->obj);
    if(ms_engine->task)
    {
        apt_task_t* task = apt_consumer_task_base_get(ms_engine->task);
        apt_task_start(task);
    }
    return mrcp_engine_open_respond(engine, TRUE);
}

/** Close recognizer engine */
static apt_bool_t ms_recog_engine_close(mrcp_engine_t* engine)
{
    ms_recog_engine_t* ms_engine = static_cast<ms_recog_engine_t*>(engine->obj);
    if(ms_engine->task)
    {
        apt_task_t* task = apt_consumer_task_base_get(ms_engine->task);
        apt_task_terminate(task, TRUE);
    }
    return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t*
ms_recog_engine_channel_create(mrcp_engine_t* engine, apr_pool_t* pool)
{
    mpf_stream_capabilities_t* capabilities;
    mpf_termination_t* termination;

    /* create recog channel */
    auto* recog_channel =
    static_cast<ms_recog_channel_t*>(apr_palloc(pool, sizeof(ms_recog_channel_t)));
    recog_channel->ms_engine = static_cast<ms_recog_engine_t*>(engine->obj);
    recog_channel->recog_request = nullptr;
    recog_channel->stop_response = nullptr;
    recog_channel->detector = mpf_activity_detector_create(pool);
    recog_channel->audio_out = nullptr;

    recog_channel->resource = nullptr;
    recog_channel->resource = new RecogResource();

    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "recognition engine: channel is created");

    capabilities = mpf_sink_stream_capabilities_create(pool);
    mpf_codec_capabilities_add(
        &capabilities->codecs, 
        MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
        "LPCM");
    // Microsoft Cognitive Speech Recognition service only supports 16k 16bit mono pcm.
    // see https://docs.microsoft.com/en-us/cpp/cognitive-services/speech/audio-audiostreamformat#getwaveformatpcm

    /* create media termination */
    termination =
    mrcp_engine_audio_termination_create(recog_channel, /* object to associate */
                                         &audio_stream_vtable, /* virtual methods table of audio stream */
                                         capabilities, /* stream capabilities */
                                         pool); /* pool to allocate memory from */

    /* create engine channel base */
    recog_channel->channel =
    mrcp_engine_channel_create(engine, /* engine */
                               &channel_vtable, /* virtual methods table of engine channel */
                               recog_channel, /* object to associate */
                               termination,   /* associated media termination */
                               pool);         /* pool to allocate memory from */

    return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t ms_recog_channel_destroy(mrcp_engine_channel_t* channel)
{
    auto* recog_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);

    // Destroy the recognizer
    if(recog_channel->resource != nullptr)
    {
        recog_channel->resource->recognizer.reset();
    }
    return TRUE;
}


/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t ms_recog_channel_open(mrcp_engine_channel_t* channel)
{
    return ms_recog_msg_signal(MS_RECOG_MSG_OPEN_CHANNEL, channel, nullptr);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t ms_recog_channel_close(mrcp_engine_channel_t* channel)
{
    return ms_recog_msg_signal(MS_RECOG_MSG_CLOSE_CHANNEL, channel, nullptr);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t ms_recog_channel_request_process(mrcp_engine_channel_t* channel,
                                                   mrcp_message_t* request)
{
    return ms_recog_msg_signal(MS_RECOG_MSG_REQUEST_PROCESS, channel, request);
}

/** Process DEFINE_GRAMMAR request */
static apt_bool_t ms_recog_channel_define_grammar(mrcp_engine_channel_t* channel,
                                                  mrcp_message_t* request,
                                                  mrcp_message_t* response)
{
    apt_log(RECOG_LOG_MARK,APT_PRIO_INFO, "DEFINE_GRAMMAR:\n\n%s\n\n", request->body.buf);

    // Get generic header
    mrcp_generic_header_t* generic_header =
    static_cast<mrcp_generic_header_t*>(mrcp_generic_header_get(request));

    if(generic_header)
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Processing generic header");

        if(mrcp_generic_header_property_check(request, GENERIC_HEADER_CONTENT_ID) == TRUE)
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Header Content-Id is [%s]",
                    generic_header->content_id.buf);

            auto* recog_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);

            // Store content-id i.e. grammarId in recognition resource.
            if(recog_channel->resource != nullptr)
            {
                // Deep copy content-id from request message.
                recog_channel->resource->grammarId.assign(generic_header->content_id.buf, (size_t)generic_header->content_id.length);
                apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Stored grammar-Id [%s] in recog resource", recog_channel->resource->grammarId.c_str());
            }
        }
        else
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_ERROR, "GENERIC_HEADER_CONTENT_ID not found in the header of DEFINE_GRAMMAR request.");
        }
    }
    else
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_ERROR, "generic header not found in DEFINE_GRAMMAR request.");
    }

    /* send asynchronous response for not handled request */
    mrcp_engine_channel_message_send(channel, response);

    return TRUE;
}

/** Process RECOGNIZE request */
static apt_bool_t ms_recog_channel_recognize(mrcp_engine_channel_t* channel,
                                             mrcp_message_t* request,
                                             mrcp_message_t* response)
{
    auto* recog_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);
    const mpf_codec_descriptor_t* descriptor = mrcp_engine_sink_stream_codec_get(channel);

    if(!descriptor)
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "Failed to Get Codec Descriptor " APT_SIDRES_FMT,
                MRCP_MESSAGE_SIDRES(request));
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }

    recog_channel->timers_started = TRUE;

    /* get recognizer header */
    mrcp_recog_header_t* recog_header =
    static_cast<mrcp_recog_header_t*>(mrcp_resource_header_get(request));
    if(recog_header)
    {
        if(mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE)
        {
            recog_channel->timers_started = recog_header->start_input_timers;
        }
        if(mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE)
        {
            mpf_activity_detector_noinput_timeout_set(recog_channel->detector,
                                                      recog_header->no_input_timeout);
        }
        if(mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE)
        {
            mpf_activity_detector_silence_timeout_set(recog_channel->detector,
                                                      recog_header->speech_complete_timeout);
        }
    }

    // Get generic headers
    mrcp_generic_header_t* generic_header =
    static_cast<mrcp_generic_header_t*>(mrcp_generic_header_get(request));
    if(generic_header)
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Processing generic header");

        if(mrcp_generic_header_property_check(request, GENERIC_HEADER_CONTENT_LENGTH) == TRUE)
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Header Content Length is [%d]",
                    generic_header->content_length);
        }

        if(mrcp_generic_header_property_check(request, GENERIC_HEADER_VENDOR_SPECIFIC_PARAMS) == TRUE)
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Vendor specific param is [%s]",
                    generic_header->vendor_specific_params);
        }
    }

    if(recog_channel->resource != nullptr)
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Receive new recognize request");
    }

    response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;

    /* send asynchronous response */
    mrcp_engine_channel_message_send(channel, response);

    recog_channel->recog_request = request;
    return TRUE;
}

/** Process STOP request */
static apt_bool_t ms_recog_channel_stop(mrcp_engine_channel_t* channel,
                                        mrcp_message_t* request,
                                        mrcp_message_t* response)
{
    /* process STOP request */
    auto* recog_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);

    /* store STOP request, make sure there is no more activity and only then send the response */
    recog_channel->stop_response = response;
    return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t ms_recog_channel_timers_start(mrcp_engine_channel_t* channel,
                                                mrcp_message_t* request,
                                                mrcp_message_t* response)
{
    auto* recog_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);
    recog_channel->timers_started = TRUE;
    return mrcp_engine_channel_message_send(channel, response);
}

/** Dispatch MRCP request */
static apt_bool_t ms_recog_channel_request_dispatch(mrcp_engine_channel_t* channel,
                                                    mrcp_message_t* request)
{
    apt_bool_t processed = FALSE;
    mrcp_message_t* response = mrcp_response_create(request, request->pool);
    switch(request->start_line.method_id)
    {
    case RECOGNIZER_SET_PARAMS:
        break;
    case RECOGNIZER_GET_PARAMS:
        break;
    case RECOGNIZER_DEFINE_GRAMMAR:
        processed = ms_recog_channel_define_grammar(channel, request, response);
        break;
    case RECOGNIZER_RECOGNIZE:
        processed = ms_recog_channel_recognize(channel, request, response);
        break;
    case RECOGNIZER_GET_RESULT:
        break;
    case RECOGNIZER_START_INPUT_TIMERS:
        processed = ms_recog_channel_timers_start(channel, request, response);
        break;
    case RECOGNIZER_STOP:
        processed = ms_recog_channel_stop(channel, request, response);
        break;
    default:
        break;
    }
    if(processed == FALSE)
    {
        /* send asynchronous response for not handled request */
        mrcp_engine_channel_message_send(channel, response);
    }
    return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t ms_recog_stream_destroy(mpf_audio_stream_t* stream)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Stream destroyed");
    return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t ms_recog_stream_open(mpf_audio_stream_t* stream, mpf_codec_t* codec)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Opening stream in direction: %d", stream->direction);

    auto recog_channel = static_cast<ms_recog_channel_t*>(stream->obj);
    auto resource = recog_channel->resource;   
    
    // Default for a stream in unimrcp
    uint8_t channels = 1;
    uint8_t bits_per_sample = 16;

    // Default for Microsoft cognitive speech service
    uint sample_rates = 8000;

    if (codec != NULL && codec->attribs != NULL)
    {
        // This is a "null mpf audio bridge" scenario where source and sink match at the codec level
        // In this case there is no transformation involved and we can directly use the only codec involved.

        sample_rates = (uint)codec->attribs->sample_rates;
        bits_per_sample = codec->attribs->bits_per_sample;

        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "CreatePushStream from codec: %d %d %d",
            sample_rates,
            bits_per_sample,
            channels);        
    }
    else if (stream->direction == STREAM_DIRECTION_SEND &&
             stream->tx_descriptor != nullptr &&
             stream->tx_descriptor->enabled)
    {
        // This is a "mpf bridge" scenario, where stream would have gone through codec transforms and
        // resampling to form a LPCM sink stream. Hence there is no single codec involved here but a daisy chain.

        sample_rates = (uint)stream->tx_descriptor->sampling_rate;

        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "CreatePushStream from send (sink) stream: %s %d %d %d %d %s",
            stream->tx_descriptor->name.buf,
            stream->tx_descriptor->payload_type,
            sample_rates,
            bits_per_sample,
            channels,
            stream->tx_descriptor->enabled ? "enabled" : "disabled");
    }
    else
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "CreatePushStream from default: %d %d %d",
            sample_rates,
            bits_per_sample,
            channels);        
    }  
    
    resource->pushStream = Audio::AudioInputStream::CreatePushStream(
            Audio::AudioStreamFormat::GetWaveFormatPCM(
                sample_rates,
                bits_per_sample,
                channels));
    const auto audioInput = Audio::AudioConfig::FromStreamInput(resource->pushStream);
    // Unique ID for the connection. Must be created unique on every connection
    // very useful when reporting issues
    resource->config->SetServiceProperty("connectionId", "a88212cb-7df6-4ee1-9011-45d944771156", ServicePropertyChannel::UriQueryParameter);
    resource->recognizer = DialogServiceConnector::FromConfig(resource->config, audioInput);

    resource->recognizer->Recognizing.Connect([](const SpeechRecognitionEventArgs& e) noexcept {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "--------------- Recognizing: %s", e.Result->Text.c_str());
    });

    // Note the recog_channel must be captured as a variable
    resource->recognizer->Recognized.Connect(
    [recog_channel](const SpeechRecognitionEventArgs& e) {
        if(e.Result->Reason == ResultReason::RecognizedSpeech)
        {
            // just log the reco result since we're interested in the bot response result
            // which will come with the ActivityReceived event handler below
            std::string displayText = e.Result->Text;
            apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "--------------- Recognized text: %s", displayText.c_str());
        }
        else if(e.Result->Reason == ResultReason::NoMatch)
        {
            auto noMatch = NoMatchDetails::FromResult(e.Result);
            switch (noMatch->Reason)
            {
                case NoMatchReason::NotRecognized:
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "NO MATCH: Speech detected but not recognized.");
                    break;
                case NoMatchReason::InitialSilenceTimeout:
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "NO MATCH: Audio stream contains only silence.");
                    break;
                case NoMatchReason::InitialBabbleTimeout:
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "NO MATCH: Audio stream contains only noise.");
                    break;
                case NoMatchReason::KeywordNotRecognized:
                    // should not happen since we do not use keyword verification
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "NO MATCH: Keyword not recognized.");
                    break;
                default:
                    // also should not happen but log just in case
                    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "NO MATCH: Unknown recognition failure.");
            }
            ms_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_NO_MATCH);
        }
    });

    resource->recognizer->ActivityReceived.Connect(
        [recog_channel](const ActivityReceivedEventArgs& e)
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "--------------- Received bot response.");

            // Note GetActivity below returns a JSON which is an array of
            // bot framework activities activities
            recog_channel->resource->result = e.GetActivity();
            ms_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
        });

    resource->recognizer->Canceled.Connect(
    [recog_channel](const SpeechRecognitionCanceledEventArgs& e) {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "--------------- Speech recognition cancelled.");
        if(e.Reason == CancellationReason::Error)
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_ERROR, "Speech recognition error, Error Code: [%d], Error Details: [%s].",
                    static_cast<int>(e.ErrorCode), e.ErrorDetails.c_str());
            apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "Did you update the subscription info?");
        }
        ms_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_ERROR);
    });

    resource->recognizer->ListenOnceAsync();
    return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t ms_recog_stream_close(mpf_audio_stream_t* stream)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Stream closed");
    return TRUE;
}

/* Load recognition result */
static apt_bool_t ms_recog_result_load(ms_recog_channel_t* recog_channel, mrcp_message_t* message)
{
    if(recog_channel->resource == nullptr)
    {
        return false;
    }

    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Load recognized result.");


    const auto body = &message->body;

    // since our result is a JSON array of BF activities we ignore it,
    // the way to handle that is TBD so we provide dummy result here
    // const auto result = recog_channel->resource->result.c_str();
    const auto grammarId = recog_channel->resource->grammarId.c_str();
    body->buf = apr_psprintf(message->pool,
                             "<?xml version=\"1.0\"?>\n"
                             "<result>\n"
                             "  <interpretation grammar=\"session:%s\" confidence=\"%.2f\">\n"
                             "    <instance>Dummy result</instance>\n"
                             "    <input mode=\"speech\">Dummy result</input>\n"
                             "  </interpretation>\n"
                             "</result>\n",
                             grammarId, 0.99f);

    if(body->buf)
    {
        mrcp_generic_header_t* generic_header;
        generic_header = mrcp_generic_header_prepare(message);
        if(generic_header)
        {
            /* set content type */
            apt_string_assign(&generic_header->content_type,
                              "application/x-nlsml", message->pool);
            mrcp_generic_header_property_add(message, GENERIC_HEADER_CONTENT_TYPE);
        }

        body->length = strlen(body->buf);
    }

    return TRUE;
}

/* Raise RECOGNITION-COMPLETE event */
static apt_bool_t ms_recog_recognition_complete(ms_recog_channel_t* recog_channel,
                                                mrcp_recog_completion_cause_e cause)
{
    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Complete recognition");

    if(recog_channel->recog_request == nullptr)
    {
        return false;
    }
    /* create RECOGNITION-COMPLETE event */
    mrcp_message_t* message =
    mrcp_event_create(recog_channel->recog_request, RECOGNIZER_RECOGNITION_COMPLETE,
                      recog_channel->recog_request->pool);
    if(!message)
    {
        return FALSE;
    }

    /* get/allocate recognizer header */
    mrcp_recog_header_t* recog_header =
    static_cast<mrcp_recog_header_t*>(mrcp_resource_header_prepare(message));
    if(recog_header)
    {
        /* set completion cause */
        recog_header->completion_cause = cause;
        mrcp_resource_header_property_add(message, RECOGNIZER_HEADER_COMPLETION_CAUSE);
    }

    /* set request state */
    message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

    if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS)
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Recognition complete: try to load recog results");
        ms_recog_result_load(recog_channel, message);
    }

    recog_channel->recog_request = nullptr;
    /* send async event */
    return mrcp_engine_channel_message_send(recog_channel->channel, message);
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t ms_recog_stream_write(mpf_audio_stream_t* stream, const mpf_frame_t* frame)
{
    auto* recog_channel = static_cast<ms_recog_channel_t*>(stream->obj);
    if(recog_channel->stop_response)
    {
        /* send asynchronous response to STOP request */
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "Send asynchronous response to STOP request");
        mrcp_engine_channel_message_send(recog_channel->channel, recog_channel->stop_response);
        recog_channel->stop_response = nullptr;
        recog_channel->recog_request = nullptr;
        return TRUE;
    }

    apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "-------------- Sending audio buffer of size: %i", frame->codec_frame.size);

    if(frame->codec_frame.size)
    {
        recog_channel->resource->pushStream->Write(static_cast<uint8_t*>(
                                                   frame->codec_frame.buffer),
                                                   frame->codec_frame.size);
    }
    else
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "Codec frame is empty");
    }

    return TRUE;
}

static apt_bool_t ms_recog_msg_signal(ms_recog_msg_type_e type,
                                      mrcp_engine_channel_t* channel,
                                      mrcp_message_t* request)
{
    apt_bool_t status = FALSE;
    auto* ms_channel = static_cast<ms_recog_channel_t*>(channel->method_obj);
    auto ms_engine = ms_channel->ms_engine;
    apt_task_t* task = apt_consumer_task_base_get(ms_engine->task);
    apt_task_msg_t* msg = apt_task_msg_get(task);
    if(msg)
    {
        msg->type = TASK_MSG_USER;
        auto* ms_msg = (ms_recog_msg_t*)msg->data;

        ms_msg->type = type;
        ms_msg->channel = channel;
        ms_msg->request = request;
        status = apt_task_msg_signal(task, msg);
    }
    return status;
}

static apt_bool_t ms_recog_msg_process(apt_task_t* task, apt_task_msg_t* msg)
{
    auto* ms_msg = (ms_recog_msg_t*)msg->data;
    switch(ms_msg->type)
    {
    case MS_RECOG_MSG_OPEN_CHANNEL:
        /* open channel and send async response */
        mrcp_engine_channel_open_respond(ms_msg->channel, TRUE);
        break;
    case MS_RECOG_MSG_CLOSE_CHANNEL:
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_INFO, "ms_recog_msg_process: channel is closed");

        /* close channel, make sure there is no activity and send async response */
        auto* recog_channel = static_cast<ms_recog_channel_t*>(ms_msg->channel->method_obj);

        if(recog_channel->resource != nullptr && recog_channel->resource->recognizer != nullptr)
        {
            recog_channel->resource->recognizer.reset();
        }

        mrcp_engine_channel_close_respond(ms_msg->channel);
        break;
    }
    case MS_RECOG_MSG_REQUEST_PROCESS:
        ms_recog_channel_request_dispatch(ms_msg->channel, ms_msg->request);
        break;
    default:
        break;
    }
    return TRUE;
}

static std::string ms_recog_get_lexical_text_from_json_result(std::string& jsonResult)
{
    std::string lexicalText;

    if (jsonResult.empty())
    {
        apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "jsonResult is empty.");
    }
    else
    {
        rapidjson::Document doc;
        doc.Parse(jsonResult.c_str());
        if(doc.IsObject())
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "jsonResult document created");

            // Using a reference for consecutive access is handy and faster.
            const rapidjson::Value& nBest = doc["NBest"];

            if (nBest.IsArray())
            {
                apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "Fetched reference to NBest array from jsonResult");

                if (nBest.Size() > 0)
                {
                    // Pick 0th result from NBest
                    rapidjson::Value::ConstValueIterator itrBestResult = nBest.Begin();
                    const rapidjson::Value::ConstMemberIterator itrBestResultLexical = itrBestResult->FindMember("Lexical");
                    lexicalText = itrBestResultLexical->value.GetString();
                    apt_log(RECOG_LOG_MARK, APT_PRIO_DEBUG, "Lexical text for best result: %s", lexicalText.c_str());
                }
                else
                {
                    apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "NBest array is empty in jsonResult");
                }
            }
            else
            {
                apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "jsonResult NBest is not an array");
            }
        }
        else
        {
            apt_log(RECOG_LOG_MARK, APT_PRIO_WARNING, "jsonResult document is not an object");
        }  
    }

    return lexicalText;
}
