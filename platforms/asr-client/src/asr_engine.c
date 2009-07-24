/*
 * Copyright 2009 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

/* APR includes */
#include <apr_thread_cond.h>
#include <apr_thread_proc.h>

/* common includes */
#include "mrcp_application.h"
#include "mrcp_session.h"
#include "mrcp_message.h"
#include "mrcp_generic_header.h"
/* recognizer includes */
#include "mrcp_recog_header.h"
#include "mrcp_recog_resource.h"
/* APT includes */
#include "apt_nlsml_doc.h"
#include "apt_log.h"
#include "apt_pool.h"

#include "asr_engine.h"


/** ASR engine on top of UniMRCP client stack */
struct asr_engine_t {
	/** MRCP client stack */
	mrcp_client_t      *mrcp_client;
	/** MRCP client stack */
	mrcp_application_t *mrcp_app;
	/** Memory pool */
	apr_pool_t         *pool;
};


/** ASR session on top of UniMRCP session/channel */
struct asr_session_t {
	/** MRCP session */
	mrcp_session_t     *mrcp_session;
	/** MRCP channel */
	mrcp_channel_t     *mrcp_channel;

	/** File to read grammar from */
	FILE               *grammar;
	/** File to read audio stream from */
	FILE               *audio_in;
	/** Streaming is in-progress */
	apt_bool_t          streaming;

	/** Thread to launch ASR scenario in */
	apr_thread_t       *thread;

	/** Conditional wait object */
	apr_thread_cond_t  *wait_object;
	/** Mutex of the wait object */
	apr_thread_mutex_t *mutex;

	/** Message sent from client stack */
	const mrcp_app_message_t *app_message;
};


/** Declaration of recognizer audio stream methods */
static apt_bool_t asr_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	NULL,
	NULL,
	NULL,
	asr_stream_read,
	NULL,
	NULL,
	NULL
};

static apt_bool_t app_message_handler(const mrcp_app_message_t *app_message);
static void* APR_THREAD_FUNC asr_session_run(apr_thread_t *thread, void *data);


/** Create ASR engine */
asr_engine_t* asr_engine_create(apt_dir_layout_t *dir_layout, apr_pool_t *pool)
{
	mrcp_client_t *mrcp_client;
	mrcp_application_t *mrcp_app;
	asr_engine_t *engine = apr_palloc(pool,sizeof(asr_engine_t));

	/* create UniMRCP client stack */
	mrcp_client = unimrcp_client_create(dir_layout);
	if(!mrcp_client) {
		return NULL;
	}
	
	/* create an application */
	mrcp_app = mrcp_application_create(
								app_message_handler,
								engine,
								pool);
	if(!mrcp_app) {
		mrcp_client_destroy(mrcp_client);
		return NULL;
	}

	/* register application in client stack */
	mrcp_client_application_register(mrcp_client,mrcp_app,"ASRAPP");

	/* start client stack */
	if(mrcp_client_start(mrcp_client) != TRUE) {
		mrcp_client_destroy(mrcp_client);
		return NULL;
	}

	engine->mrcp_client = mrcp_client;
	engine->mrcp_app = mrcp_app;
	return engine;
}

/** Destroy ASR engine */
apt_bool_t asr_engine_destroy(asr_engine_t *engine)
{
	if(engine->mrcp_client) {
		/* shutdown client stack */
		mrcp_client_shutdown(engine->mrcp_client);
		/* destroy client stack */
		mrcp_client_destroy(engine->mrcp_client);
		engine->mrcp_client = NULL;
		engine->mrcp_app = NULL;
	}
	return TRUE;
}


/** Create ASR session */
static asr_session_t* asr_session_create(asr_engine_t *engine, const char *profile)
{
	mpf_termination_t *termination;
	mrcp_channel_t *channel;
	mrcp_session_t *session;

	asr_session_t *asr_session = malloc(sizeof(asr_session_t));

	/* create session */
	session = mrcp_application_session_create(engine->mrcp_app,profile,asr_session);
	if(!session) {
		free(asr_session);
		return FALSE;
	}
	
	termination = mrcp_application_source_termination_create(
			session,                   /* session, termination belongs to */
			&audio_stream_vtable,      /* virtual methods table of audio stream */
			NULL,                      /* codec descriptor of audio stream (NULL by default) */
			asr_session);              /* object to associate */
	
	channel = mrcp_application_channel_create(
			session,                   /* session, channel belongs to */
			MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
			termination,               /* media termination, used to terminate audio stream */
			NULL,                      /* RTP descriptor, used to create RTP termination (NULL by default) */
			asr_session);              /* object to associate */

	if(!channel) {
		mrcp_application_session_destroy(session);
		free(asr_session);
		return FALSE;
	}
	
	asr_session->mrcp_session = session;
	asr_session->mrcp_channel = channel;
	asr_session->streaming = FALSE;
	asr_session->audio_in = NULL;
	asr_session->grammar = NULL;
	asr_session->thread = NULL;
	asr_session->mutex = NULL;
	asr_session->wait_object = NULL;
	asr_session->app_message = NULL;
	return asr_session;
}

/** Destroy ASR session */
static apt_bool_t asr_session_destroy(asr_session_t *asr_session, apt_bool_t terminate)
{
	if(terminate == TRUE) {
		apr_thread_mutex_lock(asr_session->mutex);
		if(mrcp_application_session_terminate(asr_session->mrcp_session) == TRUE) {
			apr_thread_cond_wait(asr_session->wait_object,asr_session->mutex);
			/* the response must be checked to be the valid one */
		}
		apr_thread_mutex_unlock(asr_session->mutex);
	}

	if(asr_session->grammar) {
		fclose(asr_session->grammar);
		asr_session->grammar = NULL;
	}

	if(asr_session->audio_in) {
		fclose(asr_session->audio_in);
		asr_session->audio_in = NULL;
	}

	if(asr_session->mutex) {
		apr_thread_mutex_destroy(asr_session->mutex);
		asr_session->mutex = NULL;
	}
	if(asr_session->wait_object) {
		apr_thread_cond_destroy(asr_session->wait_object);
		asr_session->wait_object = NULL;
	}
	if(asr_session->mrcp_session) {
		mrcp_application_session_destroy(asr_session->mrcp_session);
		asr_session->mrcp_session = NULL;
	}
	free(asr_session);
	return TRUE;
}

/** Open audio input file */
static apt_bool_t asr_input_file_open(asr_session_t *asr_session, const apt_dir_layout_t *dir_layout, const char *input_file)
{
	char *input_file_path = apt_datadir_filepath_get(dir_layout,input_file,asr_session->mrcp_session->pool);
	if(!input_file_path) {
		return FALSE;
	}
	asr_session->audio_in = fopen(input_file_path,"rb");
	if(!asr_session->audio_in) {
		apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Cannot Find [%s]",input_file_path);
		return FALSE;
	}

	return TRUE;
}

/** Open grammar file */
static apt_bool_t asr_grammar_file_open(asr_session_t *asr_session, const apt_dir_layout_t *dir_layout, const char *grammar_file)
{
	char *grammar_file_path = apt_datadir_filepath_get(dir_layout,grammar_file,asr_session->mrcp_session->pool);
	if(!grammar_file_path) {
		return FALSE;
	}
	asr_session->grammar = fopen(grammar_file_path,"r");
	if(!asr_session->grammar) {
		apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Cannot Find [%s]",grammar_file_path);
		return FALSE;
	}

	return TRUE;
}

/** Launch demo ASR session */
apt_bool_t asr_session_launch(asr_engine_t *engine, const char *grammar_file, const char *input_file, const char *profile)
{
	const apt_dir_layout_t *dir_layout = mrcp_application_dir_layout_get(engine->mrcp_app);
	asr_session_t *asr_session = asr_session_create(engine,profile);
	if(!asr_session) {
		return FALSE;
	}

	if(asr_input_file_open(asr_session,dir_layout,input_file) == FALSE ||
		asr_grammar_file_open(asr_session,dir_layout,grammar_file) == FALSE) {
		asr_session_destroy(asr_session,FALSE);
		return FALSE;
	}

	/* Launch a thread to run demo ASR session in */
	if(apr_thread_create(&asr_session->thread,NULL,asr_session_run,asr_session,asr_session->mrcp_session->pool) != APR_SUCCESS) {
		asr_session_destroy(asr_session,FALSE);
		return FALSE;
	}
	
	return TRUE;
}

/** MPF callback to read audio frame */
static apt_bool_t asr_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	asr_session_t *asr_session = stream->obj;
	if(asr_session && asr_session->streaming == TRUE) {
		if(asr_session->audio_in) {
			if(fread(frame->codec_frame.buffer,1,frame->codec_frame.size,asr_session->audio_in) == frame->codec_frame.size) {
				/* normal read */
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
			}
			else {
				/* file is over */
				asr_session->streaming = FALSE;
			}
		}
	}
	return TRUE;
}

/** Create DEFINE-GRAMMAR request */
static mrcp_message_t* define_grammar_message_create(asr_session_t *asr_session)
{
	/* create MRCP message */
	mrcp_message_t *mrcp_message = mrcp_application_message_create(
						asr_session->mrcp_session,
						asr_session->mrcp_channel,
						RECOGNIZER_DEFINE_GRAMMAR);
	if(mrcp_message) {
		mrcp_generic_header_t *generic_header;
		/* get/allocate generic header */
		generic_header = mrcp_generic_header_prepare(mrcp_message);
		if(generic_header) {
			/* set generic header fields */
			if(mrcp_message->start_line.version == MRCP_VERSION_2) {
				apt_string_assign(&generic_header->content_type,"application/srgs+xml",mrcp_message->pool);
			}
			else {
				apt_string_assign(&generic_header->content_type,"application/grammar+xml",mrcp_message->pool);
			}
			mrcp_generic_header_property_add(mrcp_message,GENERIC_HEADER_CONTENT_TYPE);
			apt_string_assign(&generic_header->content_id,"demo-grammar",mrcp_message->pool);
			mrcp_generic_header_property_add(mrcp_message,GENERIC_HEADER_CONTENT_ID);
		}
		/* set message body */
		if(asr_session->grammar) {
			char text[1024];
			apr_size_t size;
			size = fread(text,1,sizeof(text),asr_session->grammar);
			apt_string_assign_n(&mrcp_message->body,text,size,mrcp_message->pool);
		}
	}
	return mrcp_message;
}

/** Create RECOGNIZE request */
static mrcp_message_t* recognize_message_create(asr_session_t *asr_session)
{
	/* create MRCP message */
	mrcp_message_t *mrcp_message = mrcp_application_message_create(
										asr_session->mrcp_session,
										asr_session->mrcp_channel,
										RECOGNIZER_RECOGNIZE);
	if(mrcp_message) {
		mrcp_recog_header_t *recog_header;
		mrcp_generic_header_t *generic_header;
		/* get/allocate generic header */
		generic_header = mrcp_generic_header_prepare(mrcp_message);
		if(generic_header) {
			/* set generic header fields */
			apt_string_assign(&generic_header->content_type,"text/uri-list",mrcp_message->pool);
			mrcp_generic_header_property_add(mrcp_message,GENERIC_HEADER_CONTENT_TYPE);
			/* set message body */
			apt_string_assign(&mrcp_message->body,"session:demo-grammar",mrcp_message->pool);
		}
		/* get/allocate recognizer header */
		recog_header = mrcp_resource_header_prepare(mrcp_message);
		if(recog_header) {
			if(mrcp_message->start_line.version == MRCP_VERSION_2) {
				/* set recognizer header fields */
				recog_header->cancel_if_queue = FALSE;
				mrcp_resource_header_property_add(mrcp_message,RECOGNIZER_HEADER_CANCEL_IF_QUEUE);
			}
			recog_header->no_input_timeout = 5000;
			mrcp_resource_header_property_add(mrcp_message,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT);
			recog_header->recognition_timeout = 10000;
			mrcp_resource_header_property_add(mrcp_message,RECOGNIZER_HEADER_RECOGNITION_TIMEOUT);
			recog_header->start_input_timers = TRUE;
			mrcp_resource_header_property_add(mrcp_message,RECOGNIZER_HEADER_START_INPUT_TIMERS);
			recog_header->confidence_threshold = 0.87f;
			mrcp_resource_header_property_add(mrcp_message,RECOGNIZER_HEADER_CONFIDENCE_THRESHOLD);
		}
	}
	return mrcp_message;
}
/** Parse NLSML result */
static apt_bool_t nlsml_result_parse(mrcp_message_t *message)
{
	apr_xml_elem *interpret;
	apr_xml_elem *instance;
	apr_xml_elem *input;
	apr_xml_doc *doc = nlsml_doc_load(&message->body,message->pool);
	if(!doc) {
		return FALSE;
	}
	
	/* walk through interpreted results */
	interpret = nlsml_first_interpret_get(doc);
	for(; interpret; interpret = nlsml_next_interpret_get(interpret)) {
		/* get instance and input */
		nlsml_interpret_results_get(interpret,&instance,&input);
		if(instance) {
			/* process instance */
			if(instance->first_cdata.first) {
				apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Interpreted Instance [%s]",instance->first_cdata.first->text);
			}
		}
		if(input) {
			/* process input */
			if(input->first_cdata.first) {
				apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Interpreted Input [%s]",input->first_cdata.first->text);
			}
		}
	}
	return TRUE;
}

/** Application message handler */
static apt_bool_t app_message_handler(const mrcp_app_message_t *app_message)
{
	if((app_message->message_type == MRCP_APP_MESSAGE_TYPE_SIGNALING && 
		app_message->sig_message.message_type == MRCP_SIG_MESSAGE_TYPE_RESPONSE) ||
		app_message->message_type == MRCP_APP_MESSAGE_TYPE_CONTROL) {

		asr_session_t *asr_session = mrcp_application_session_object_get(app_message->session);
		if(asr_session) {
			apr_thread_mutex_lock(asr_session->mutex);
			asr_session->app_message = app_message;
			apr_thread_cond_signal(asr_session->wait_object);
			apr_thread_mutex_unlock(asr_session->mutex);
		}
	}
	return TRUE;
}

/** Check signaling response */
static apt_bool_t sig_response_check(const mrcp_app_message_t *app_message)
{
	if(!app_message || app_message->message_type != MRCP_APP_MESSAGE_TYPE_SIGNALING) {
		return FALSE;
	}

	return (app_message->sig_message.status == MRCP_SIG_STATUS_CODE_SUCCESS) ? TRUE : FALSE;
}

/** Check MRCP response */
static apt_bool_t mrcp_response_check(const mrcp_app_message_t *app_message, mrcp_request_state_e state)
{
	mrcp_message_t *mrcp_message = NULL;
	if(app_message && app_message->message_type == MRCP_APP_MESSAGE_TYPE_CONTROL) {
		mrcp_message = app_message->control_message;
	}

	if(!mrcp_message || mrcp_message->start_line.message_type != MRCP_MESSAGE_TYPE_RESPONSE ) {
		return FALSE;
	}
	return (mrcp_message->start_line.request_state == state) ? TRUE : FALSE;
}

/** Get MRCP event */
static mrcp_message_t* mrcp_event_get(const mrcp_app_message_t *app_message)
{
	mrcp_message_t *mrcp_message = NULL;
	if(app_message && app_message->message_type == MRCP_APP_MESSAGE_TYPE_CONTROL) {
		mrcp_message = app_message->control_message;
	}

	if(!mrcp_message || mrcp_message->start_line.message_type != MRCP_MESSAGE_TYPE_EVENT) {
		return NULL;
	}
	return mrcp_message;
}

/** Thread function to run ASR scenario in */
static void* APR_THREAD_FUNC asr_session_run(apr_thread_t *thread, void *data)
{
	asr_session_t *asr_session = data;
	const mrcp_app_message_t *app_message;
	mrcp_message_t *mrcp_message;

	/* Create cond wait object and mutex */
	apr_thread_mutex_create(&asr_session->mutex,APR_THREAD_MUTEX_DEFAULT,asr_session->mrcp_session->pool);
	apr_thread_cond_create(&asr_session->wait_object,asr_session->mrcp_session->pool);

	/* 1. Send add channel request and wait for the response */
	apr_thread_mutex_lock(asr_session->mutex);
	app_message = NULL;
	if(mrcp_application_channel_add(asr_session->mrcp_session,asr_session->mrcp_channel) == TRUE) {
		apr_thread_cond_wait(asr_session->wait_object,asr_session->mutex);
		app_message = asr_session->app_message;
		asr_session->app_message = NULL;
	}
	apr_thread_mutex_unlock(asr_session->mutex);

	if(sig_response_check(app_message) == FALSE) {
		asr_session_destroy(asr_session,TRUE);
		return NULL;
	}

	/* 2. Send DEFINE-GRAMMAR request and wait for the response */
	apr_thread_mutex_lock(asr_session->mutex);
	app_message = NULL;
	mrcp_message = define_grammar_message_create(asr_session);
	if(mrcp_message) {
		if(mrcp_application_message_send(asr_session->mrcp_session,asr_session->mrcp_channel,mrcp_message) == TRUE) {
			apr_thread_cond_wait(asr_session->wait_object,asr_session->mutex);
			app_message = asr_session->app_message;
			asr_session->app_message = NULL;
		}
	}
	apr_thread_mutex_unlock(asr_session->mutex);

	if(mrcp_response_check(app_message,MRCP_REQUEST_STATE_COMPLETE) == FALSE) {
		asr_session_destroy(asr_session,TRUE);
		return NULL;
	}

	/* 3. Send RECOGNIZE request and wait for the response */
	apr_thread_mutex_lock(asr_session->mutex);
	app_message = NULL;
	mrcp_message = recognize_message_create(asr_session);
	if(mrcp_message) {
		if(mrcp_application_message_send(asr_session->mrcp_session,asr_session->mrcp_channel,mrcp_message) == TRUE) {
			apr_thread_cond_wait(asr_session->wait_object,asr_session->mutex);
			app_message = asr_session->app_message;
			asr_session->app_message = NULL;
		}
	}
	apr_thread_mutex_unlock(asr_session->mutex);

	if(mrcp_response_check(app_message,MRCP_REQUEST_STATE_INPROGRESS) == FALSE) {
		asr_session_destroy(asr_session,TRUE);
		return NULL;
	}
	
	/* 4. Start streaming */
	asr_session->streaming = TRUE;

	/* 5. Wait for events either START-OF-INPUT or RECOGNITION-COMPLETE */
	do {
		apr_thread_mutex_lock(asr_session->mutex);
		app_message = NULL;
		if(apr_thread_cond_timedwait(asr_session->wait_object,asr_session->mutex, 60 * 1000000) != APR_SUCCESS) {
			mrcp_message = NULL;
			apr_thread_mutex_unlock(asr_session->mutex);
			break;
		}
		app_message = asr_session->app_message;
		asr_session->app_message = NULL;
		apr_thread_mutex_unlock(asr_session->mutex);

		mrcp_message = mrcp_event_get(app_message);
	}
	while(!mrcp_message || mrcp_message->start_line.method_id != RECOGNIZER_RECOGNITION_COMPLETE);

	/* 6. Get results */
	if(mrcp_message) {
		nlsml_result_parse(mrcp_message);
	}

	/* 7. Send terminate session request, wait for the response and destroy session */
	asr_session_destroy(asr_session,TRUE);
	return NULL;
}
