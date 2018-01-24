#include "codec.h"
#include <glib.h>
#include <assert.h>
#include "call.h"
#include "log.h"
#include "rtplib.h"
#include "codeclib.h"




static codec_handler_func handler_func_stub;
static codec_handler_func handler_func_transcode;


static struct codec_handler codec_handler_stub = {
	.rtp_payload_type = -1,
	.func = handler_func_stub,
};



static void __handler_shutdown(struct codec_handler *handler) {
	if (handler->decoder)
		decoder_close(handler->decoder);
	handler->decoder = NULL;
}

static void __make_stub(struct codec_handler *handler) {
	__handler_shutdown(handler);
	handler->func = handler_func_stub;
}

static void __codec_handler_free(void *pp) {
	struct codec_handler *h = pp;
	__handler_shutdown(h);
	g_slice_free1(sizeof(*h), h);
}

static void __make_transcoder(struct codec_handler *handler, struct rtp_payload_type *source,
		struct rtp_payload_type *dest)
{
	assert(source->codec_def != NULL);
	assert(dest->codec_def != NULL);

	__handler_shutdown(handler);

	handler->func = handler_func_transcode;
	handler->decoder = decoder_new_fmt(source->codec_def, source->clock_rate, 1, 0);
	if (!handler->decoder)
		goto err;

	ilog(LOG_DEBUG, "Created transcode context for '" STR_FORMAT "' -> '" STR_FORMAT "'",
			STR_FMT(&source->encoding), STR_FMT(&dest->encoding));

	return;

err:
	__make_stub(handler);
}

// call must be locked in W
void codec_handlers_update(struct call_media *receiver, struct call_media *sink) {
	if (!receiver->codec_handlers)
		receiver->codec_handlers = g_hash_table_new_full(g_int_hash, g_int_equal,
				NULL, __codec_handler_free);

	MEDIA_CLEAR(receiver, TRANSCODE);

	// we go through the list of codecs that the receiver supports and compare it
	// with the list of codecs supported by the sink. if the receiver supports
	// a codec that the sink doesn't support, we must transcode.
	//
	// if we transcode, we transcode to the highest-preference supported codec
	// that the sink specified. determine this first.
	struct rtp_payload_type *pref_dest_codec = NULL;
	for (GList *l = sink->codecs_prefs_send.head; l; l = l->next) {
		struct rtp_payload_type *pt = l->data;
		if (!pt->codec_def)
			pt->codec_def = codec_find(&pt->encoding);
		if (!pt->codec_def) // not supported, next
			continue;
		ilog(LOG_DEBUG, "Default sink codec is " STR_FORMAT, STR_FMT(&pt->encoding));
		pref_dest_codec = pt;
		break;
	}

	for (GList *l = receiver->codecs_prefs_recv.head; l; l = l->next) {
		struct rtp_payload_type *pt = l->data;

		// first, make sure we have a codec_handler struct for this
		struct codec_handler *handler;
		handler = g_hash_table_lookup(receiver->codec_handlers, &pt->payload_type);
		if (!handler) {
			ilog(LOG_DEBUG, "Creating codec handler for " STR_FORMAT, STR_FMT(&pt->encoding));
			handler = g_slice_alloc0(sizeof(*handler));
			handler->rtp_payload_type = pt->payload_type;
			g_hash_table_insert(receiver->codec_handlers, &handler->rtp_payload_type,
					handler);
		}

		// if the sink's codec preferences are unknown (empty), or there are
		// no supported codecs to transcode to, then we have nothing
		// to do. most likely this is an initial offer without a received answer.
		// we default to forwarding without transcoding.
		if (!pref_dest_codec) {
			ilog(LOG_DEBUG, "No known/supported sink codec for " STR_FORMAT, STR_FMT(&pt->encoding));
			__make_stub(handler);
			continue;
		}

		if (g_hash_table_lookup(sink->codec_names, &pt->encoding)) {
			// the sink supports this codec. forward without transcoding.
			// XXX check format parameters as well
			ilog(LOG_DEBUG, "Sink supports codec " STR_FORMAT, STR_FMT(&pt->encoding));
			__make_stub(handler);
			continue;
		}

		// the sink does not support this codec -> transcode
		ilog(LOG_DEBUG, "Sink does not support codec " STR_FORMAT, STR_FMT(&pt->encoding));
		MEDIA_SET(receiver, TRANSCODE);
		__make_transcoder(handler, pt, pref_dest_codec);
	}
}

// call must be locked in R
struct codec_handler *codec_handler_get(struct call_media *m, int payload_type) {
	struct codec_handler *h;

	if (payload_type < 0)
		goto out;

	h = g_atomic_pointer_get(&m->codec_handler_cache);
	if (G_LIKELY(G_LIKELY(h) && G_LIKELY(h->rtp_payload_type == payload_type)))
		return h;

	h = g_hash_table_lookup(m->codec_handlers, &payload_type);
	if (!h)
		goto out;

	g_atomic_pointer_set(&m->codec_handler_cache, h);

	return h;

out:
	return &codec_handler_stub;
}

void codec_handlers_free(struct call_media *m) {
	g_hash_table_destroy(m->codec_handlers);
	m->codec_handlers = NULL;
	m->codec_handler_cache = NULL;
}


static int handler_func_stub(struct codec_handler *h, struct call_media *media, const str *s, GQueue *out) {
	struct codec_packet *p = g_slice_alloc(sizeof(*p));
	p->s = *s;
	p->free_func = NULL;
	g_queue_push_tail(out, p);
	return 0;
}
static int handler_func_transcode(struct codec_handler *h, struct call_media *media, const str *s, GQueue *out) {
	return 0;
}

void codec_packet_free(void *pp) {
	struct codec_packet *p = pp;
	if (p->free_func)
		p->free_func(p->s.s);
	g_slice_free1(sizeof(*p), p);
}



static struct rtp_payload_type *codec_make_payload_type(const str *codec) {
	const codec_def_t *dec = codec_find(codec);
	if (!dec)
		return NULL;
	const struct rtp_payload_type *rfc_pt = rtp_get_rfc_codec(codec);
	if (!rfc_pt)
		return NULL; // XXX amend for other codecs

	struct rtp_payload_type *ret = g_slice_alloc(sizeof(*ret));
	*ret = *rfc_pt;
	ret->codec_def = dec;

	return ret;
}


static struct rtp_payload_type *codec_add_payload_type(const str *codec, struct call_media *media) {
	struct rtp_payload_type *pt = codec_make_payload_type(codec);
	if (!pt) {
		ilog(LOG_WARN, "Codec '" STR_FORMAT "' requested for transcoding is not supported",
				STR_FMT(codec));
		return NULL;
	}
	// find an unused payload type number
	if (pt->payload_type < 0)
		pt->payload_type = 96; // default first dynamic payload type number
	while (1) {
		if (!g_hash_table_lookup(media->codecs, &pt->payload_type))
			break; // OK
		pt->payload_type++;
		if (pt->payload_type < 96) // if an RFC type was taken already
			pt->payload_type = 96;
		else if (pt->payload_type >= 128) {
			ilog(LOG_WARN, "Ran out of RTP payload type numbers while adding codec '"
					STR_FORMAT "' for transcoding",
				STR_FMT(codec));
			payload_type_free(pt);
			return NULL;
		}
	}
	return pt;
}









static void __rtp_payload_type_dup(struct call *call, struct rtp_payload_type *pt) {
	/* we must duplicate the contents */
	call_str_cpy(call, &pt->encoding_with_params, &pt->encoding_with_params);
	call_str_cpy(call, &pt->encoding, &pt->encoding);
	call_str_cpy(call, &pt->encoding_parameters, &pt->encoding_parameters);
	call_str_cpy(call, &pt->format_parameters, &pt->format_parameters);
}
// consumes 'pt'
static struct rtp_payload_type *__rtp_payload_type_add_recv(struct call_media *media,
		struct rtp_payload_type *pt)
{
	struct rtp_payload_type *existing_pt;
	if ((existing_pt = g_hash_table_lookup(media->codecs, &pt->payload_type))) {
		// collision/duplicate - ignore
		payload_type_free(pt);
		return existing_pt;
	}
	g_hash_table_replace(media->codecs, &pt->payload_type, pt);

	GQueue *q = g_hash_table_lookup_queue_new(media->codec_names, &pt->encoding);
	g_queue_push_tail(q, GUINT_TO_POINTER(pt->payload_type));

	g_queue_push_tail(&media->codecs_prefs_recv, pt);

	return pt;
}
// duplicates 'pt'
static void __rtp_payload_type_add_send(struct call_media *other_media, struct rtp_payload_type *pt) {
	// for the other side, we need a new 'pt' struct
	struct rtp_payload_type *pt_copy = g_slice_alloc(sizeof(*pt));
	*pt_copy = *pt;
	g_queue_push_tail(&other_media->codecs_prefs_send, pt_copy);

	// make sure we have at least an empty queue here to indicate support for this code.
	// don't add anything to the queue as we don't know the reverse RTP payload type.
	g_hash_table_lookup_queue_new(other_media->codec_names, &pt->encoding);
}
// consumes 'pt'
static void __rtp_payload_type_add(struct call_media *media, struct call_media *other_media,
		struct rtp_payload_type *pt)
{
	// if this payload type is already present in the 'codec' table, the _recv
	// function frees its argument and returns the existing entry instead.
	// otherwise it returns its argument.
	pt = __rtp_payload_type_add_recv(media, pt);
	__rtp_payload_type_add_send(other_media, pt);
}

static void __payload_queue_free(void *qq) {
	GQueue *q = qq;
	g_queue_free_full(q, (GDestroyNotify) payload_type_free);
}
static int __revert_codec_strip(GHashTable *removed, const str *codec,
		struct call_media *media, struct call_media *other_media) {
	GQueue *q = g_hash_table_lookup(removed, codec);
	if (!q)
		return 0;
	ilog(LOG_DEBUG, "Restoring codec '" STR_FORMAT "' from stripped codecs (%u payload types)",
			STR_FMT(codec), q->length);
	g_hash_table_steal(removed, codec);
	for (GList *l = q->head; l; l = l->next) {
		struct rtp_payload_type *pt = l->data;
		__rtp_payload_type_add(media, other_media, pt);
	}
	g_queue_free(q);
	return 1;
}
void codec_rtp_payload_types(struct call_media *media, struct call_media *other_media,
		GQueue *types, GHashTable *strip,
		const GQueue *offer, const GQueue *transcode)
{
	// 'media' = receiver of this offer/answer; 'other_media' = sender of this offer/answer
	struct call *call = media->call;
	struct rtp_payload_type *pt;
	static const str str_all = STR_CONST_INIT("all");
	GHashTable *removed = g_hash_table_new_full(str_hash, str_equal, NULL, __payload_queue_free);
	int remove_all = 0;

	// start fresh
	g_queue_clear(&media->codecs_prefs_recv);
	g_queue_clear_full(&other_media->codecs_prefs_send, (GDestroyNotify) payload_type_free);
	g_hash_table_remove_all(media->codecs);
	g_hash_table_remove_all(media->codec_names);

	if (strip && g_hash_table_lookup(strip, &str_all))
		remove_all = 1;

	/* we steal the entire list to avoid duplicate allocs */
	while ((pt = g_queue_pop_head(types))) {
		__rtp_payload_type_dup(call, pt); // this takes care of string allocation

		// codec stripping
		if (strip) {
			if (remove_all || g_hash_table_lookup(strip, &pt->encoding)) {
				ilog(LOG_DEBUG, "Stripping codec '" STR_FORMAT "'", STR_FMT(&pt->encoding));
				GQueue *q = g_hash_table_lookup_queue_new(removed, &pt->encoding);
				g_queue_push_tail(q, pt);
				continue;
			}
		}
		__rtp_payload_type_add(media, other_media, pt);
	}

	// now restore codecs that have been removed, but should be offered
	for (GList *l = offer ? offer->head : NULL; l; l = l->next) {
		str *codec = l->data;
		__revert_codec_strip(removed, codec, media, other_media);
	}

	// add transcode codecs
	for (GList *l = transcode ? transcode->head : NULL; l; l = l->next) {
		str *codec = l->data;
		// if we wish to 'transcode' to a codec that was offered originally,
		// simply restore it from the original list and handle it the same way
		// as 'offer'
		if (__revert_codec_strip(removed, codec, media, other_media))
			continue;
		// also check if maybe the codec was never stripped
		if (g_hash_table_lookup(media->codec_names, codec)) {
			ilog(LOG_DEBUG, "Codec '" STR_FORMAT "' requested for transcoding is already present",
					STR_FMT(codec));
			continue;
		}

		// create new payload type
		pt = codec_add_payload_type(codec, media);
		if (!pt)
			continue;

		ilog(LOG_DEBUG, "Codec '" STR_FORMAT "' added for transcoding with payload type %u",
				STR_FMT(codec), pt->payload_type);
		__rtp_payload_type_add_recv(media, pt);
	}

	g_hash_table_destroy(removed);
}
