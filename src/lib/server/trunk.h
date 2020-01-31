#pragma once
/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file src/lib/server/trunk.c
 * @brief A management API for bonding multiple connections together.
 *
 * @copyright 2019 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2019 The FreeRADIUS server project
 */
RCSIDH(server_trunk_h, "$Id$")

#include <freeradius-devel/server/connection.h>
#include <freeradius-devel/server/request.h>
#include <freeradius-devel/server/cf_parse.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fr_trunk_request_s fr_trunk_request_t;
typedef struct fr_trunk_connection_s fr_trunk_connection_t;
typedef struct fr_trunk_s fr_trunk_t;

/** Common configuration parameters for a trunk
 *
 */
typedef struct {
	fr_connection_conf_t const *conn_conf;		//!< Connection configuration.

	uint16_t		start;			//!< How many connections to start.

	uint16_t		min;			//!< Shouldn't let connections drop below this number.

	uint16_t		max;			//!< Maximum number of connections in the trunk.

	uint16_t		connecting;		//!< Maximum number of connections that can be in the
							///< connecting state.  Used to throttle connection spawning.

	uint32_t		target_req_per_conn;	//!< How many pending requests should ideally be
							///< running on each connection.  Averaged across
							///< the 'active' set of connections.

	uint32_t		max_req_per_conn;	//!< Maximum connections per request.
							///< Used to determine if we need to create new connections
							///< and whether we can enqueue new requests.

	uint64_t		max_uses;		//!< The maximum time a connection can be used.

	fr_time_delta_t		lifetime;		//!< Time between reconnects.

	fr_time_delta_t		open_delay;		//!< How long we must be above target utilisation
							///< to spawn a new connection.

	fr_time_delta_t		close_delay;		//!< How long we must be below target utilisation
							///< to close an existing connection.


	fr_time_delta_t		req_cleanup_delay;	//!< How long must a request in the unassigned (free)
							///< list not have been used for before it's cleaned up
							///< and actually freed.

	fr_time_delta_t		manage_interval;	//!< How often we run the management algorithm to
							///< open/close connections.

	unsigned		req_pool_headers;	//!< How many chunk headers the talloc pool allocated
							///< with the treq should contain.

	size_t			req_pool_size;		//!< The size of the talloc pool allocated with the treq.

	bool			always_writable;	//!< Set to true if our ability to write requests to
							///< a connection handle is not dependant on the state
							///< of the underlying connection, i.e. if the library
							///< used to implement the connection can always receive
							///< and buffer new requests irrespective of the state
							///< of the underlying socket.
							///< If this is true, #fr_trunk_connection_signal_writable
							///< does not need to be called, and requests will be
							///< enqueued as soon as they're received.
} fr_trunk_conf_t;

/** Reasons for a request being cancelled
 *
 */
typedef enum {
	FR_TRUNK_CANCEL_REASON_NONE = 0,		//!< Request has not been cancelled.
	FR_TRUNK_CANCEL_REASON_SIGNAL,			//!< Request cancelled due to a signal.
	FR_TRUNK_CANCEL_REASON_MOVE,			//!< Request cancelled because it's being moved.
	FR_TRUNK_CANCEL_REASON_REQUEUE			//!< A previously sent request is being requeued.
} fr_trunk_cancel_reason_t;

/** What type of I/O events the trunk connection is currently interested in receiving
 *
 */
typedef enum {
	FR_TRUNK_CONN_EVENT_NONE = 0x00,		//!< Don't notify the trunk on connection state
							///< changes.
	FR_TRUNK_CONN_EVENT_READ = 0x01,		//!< Trunk should be notified if a connection is
							///< readable.
	FR_TRUNK_CONN_EVENT_WRITE = 0x02,		//!< Trunk should be notified if a connection is
							///< writable.
	FR_TRUNK_CONN_EVENT_BOTH = 0x03,		//!< Trunk should be notified if a connection is
							///< readable or writable.

} fr_trunk_connection_event_t;

/** Used for sanity checks and to track which list the connection is in
 *
 */
typedef enum {
	FR_TRUNK_CONN_HALTED		= 0x00,		//!< In the initial state.
	FR_TRUNK_CONN_CONNECTING	= 0x01,		//!< Connection is connecting.
	FR_TRUNK_CONN_ACTIVE		= 0x02,		//!< Connection is connected and ready to service requests.
							///< This is active and not 'connected', because a connection
							///< can be 'connected' and 'full' or 'connected' and 'active'.
	FR_TRUNK_CONN_FAILED		= 0x04,		//!< Connection failed.  We now wait for it to enter the
							///< closed state.
	FR_TRUNK_CONN_CLOSED		= 0x08,		//!< Connection was closed, either explicitly or due to failure.
	FR_TRUNK_CONN_INACTIVE		= 0x10,		//!< Connection is inactive and can't accept any more requests.
	FR_TRUNK_CONN_DRAINING		= 0x20,		//!< Connection will be closed once it has no more outstanding
							///< requests, if it's not reactivated.
	FR_TRUNK_CONN_DRAINING_TO_FREE	= 0x40		//!< Connection will be closed once it has no more outstanding
							///< requests.
} fr_trunk_connection_state_t;

/** All connection states
 *
 */
#define FR_TRUNK_CONN_ALL \
(\
	FR_TRUNK_CONN_CONNECTING | \
	FR_TRUNK_CONN_ACTIVE | \
	FR_TRUNK_CONN_FAILED | \
	FR_TRUNK_CONN_CLOSED | \
	FR_TRUNK_CONN_INACTIVE | \
	FR_TRUNK_CONN_DRAINING | \
	FR_TRUNK_CONN_DRAINING_TO_FREE \
)

typedef enum {
	FR_TRUNK_ENQUEUE_IN_BACKLOG = 1,		//!< Request should be enqueued in backlog
	FR_TRUNK_ENQUEUE_OK = 0,			//!< Operation was successful.
	FR_TRUNK_ENQUEUE_NO_CAPACITY = -1,		//!< At maximum number of connections,
							///< and no connection has capacity.
	FR_TRUNK_ENQUEUE_DST_UNAVAILABLE = -2,		//!< Destination is down.
	FR_TRUNK_ENQUEUE_FAIL = -3			//!< General failure.
} fr_trunk_enqueue_t;

/** Config parser definitions to populate a fr_trunk_conf_t
 *
 */
extern CONF_PARSER const fr_trunk_config[];

/** Allocate a new connection for the trunk
 *
 * The trunk code only interacts with underlying connections via the connection API.
 * As a result the trunk API is shielded from the implementation details of opening
 * and closing connections.
 *
 * When creating new connections, this callback is used to allocate and configure
 * a new #fr_connection_t, this #fr_connection_t and the fr_connection API is how the
 * trunk signals the underlying connection that it should start, reconnect, and halt (stop).
 *
 * The trunk must be informed when the underlying connection is readable, and,
 * if `always_writable == false`, when the connection is writable.
 *
 * When the connection is readable, a read I/O handler installed by the init()
 * callback of the #fr_connection_t must either:
 *
 * - If there's no underlying I/O library, call `fr_trunk_connection_signal_readable(tconn)`
 *   immediately, relying on the trunk demux callback to perform decoding and demuxing.
 * - If there is an underlying I/O library, feed any incoming data to that library and
 *   then call #fr_trunk_connection_signal_readable if the underlying I/O library
 *   indicates complete responses are ready for processing.
 *
 * When the connection is writable a write I/O handler installed by the open() callback
 * of the #fr_connection_t must either:
 *
 * - If `always_writable == true` - Inform the underlying I/O library that the connection
 *   is writable.  The trunk API does not need to be informed as it will immediately pass
 *   through any enqueued requests to the I/O library.
 * - If `always_writable == false` and there's an underlying I/O library,
 *   call `fr_trunk_connection_signal_writable(tconn)` to allow the trunk mux callback
 *   to pass requests to the underlying I/O library and (optionally) signal the I/O library
 *   that the connection is writable.
 * - If `always_writable == false` and there's no underlying I/O library,
 *   call `fr_trunk_connection_signal_writable(tconn)` to allow the trunk mux callback
 *   to encode and write requests to a socket.
 *
 * @param[in] tconn		The trunk connection this connection will be bound to.
 *				Should be used as the context for any #fr_connection_t
 *				allocated.
 * @param[in] el		The event list to use for I/O and timer events.
 * @param[in] conf		Configuration of the #fr_connection_t.
 * @param[in] log_prefix	What to prefix connection log messages with.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 * @return
 *	- A new fr_connection_t on success (should be in the halted state - the default).
 *	- NULL on error.
 */
typedef fr_connection_t *(*fr_trunk_connection_alloc_t)(fr_trunk_connection_t *tconn, fr_event_list_t *el,
							fr_connection_conf_t const *conf,
							char const *log_prefix, void *uctx);

/** Inform the trunk API client which I/O events the trunk wants to receive
 *
 * I/O handlers installed by this callback should call one or more of the following
 * functions to signal that an I/O event has occurred:
 *
 * - fr_trunk_connection_signal_writable - Connection is now writable.
 * - fr_trunk_connection_signal_readable - Connection is now readable.
 * - fr_trunk_connection_signal_inactive - Connection is full or congested.
 * - fr_trunk_connection_signal_active - Connection is no longer full or congested.
 * - fr_trunk_connection_signal_reconnect - Connection is inviable and should be reconnected.
 *
 * @param[in] tconn		That should be notified of I/O events.
 * @param[in] conn		The #fr_connection_t bound to the tconn.
 *				Use #fr_connection_get_handle to access the
 *				connection handle or file descriptor.
 * @param[in] el		to insert I/O events into.
 * @param[in] notify_on		I/O events to signal the trunk connection on.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_connection_notify_t)(fr_trunk_connection_t *tconn, fr_connection_t *conn,
					     fr_event_list_t *el,
					     fr_trunk_connection_event_t notify_on, void *uctx);

/** Multiplex one or more requests into a single connection
 *
 * This callback should:
 *
 * - Pop one or more requests from the trunk connection's pending queue using
 *   #fr_trunk_connection_pop_request.
 * - Serialize the protocol request data contained within the trunk request's (treq's)
 *   pctx, writing it to the provided #fr_connection_t (or underlying connection handle).
 * - Insert the provided treq
 *   into a tracking structure associated with the #fr_connection_t or uctx.
 *   This tracking structure will be used later in the trunk demux callback to match
 *   protocol requests with protocol responses.
 *
 * If working at the socket level and a write on a file descriptor indicates
 * less data was written than was needed, the trunk API client should track the
 * amount of data written in the protocol request (preq), and should call
 * `fr_trunk_request_signal_partial(treq)`.
 * #fr_trunk_request_signal_partial will move the request out of the pending
 * queue, and store it in the partial slot of the trunk connection.
 * The next time #fr_trunk_connection_pop_request is called, the partially written
 * treq will be returned first.  The API client should continue writing the partially
 * written request to the socket.
 *
 * After calling #fr_trunk_request_signal_partial this callback *MUST NOT*
 * call #fr_trunk_connection_pop_request again, and should immediately return.
 *
 * If the request can't be written to the connection because it the connection
 * has become unusable, this callback should call
 * `fr_connection_signal_reconnect(conn)` to notify the connection API that the
 * connection is unusable. The current request will either fail, or be
 * re-enqueued depending on the trunk configuration.
 *
 * After calling #fr_connection_signal_reconnect this callback *MUST NOT*
 * call #fr_trunk_connection_pop_request again, and should immediately return.
 *
 * If the protocol request data can't be written to the connection because the
 * data is invalid or because some other error occurred, this callback should
 * call `fr_trunk_request_signal_fail(treq)`, this callback may then continue
 * popping/processing other requests.
 *
 * @param[in] tconn		The trunk connection to dequeue trunk
 *      			requests from.
 * @param[in] conn		Connection to write the request to.
 *				Use #fr_connection_get_handle to access the
 *				connection handle or file descriptor.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_request_mux_t)(fr_trunk_connection_t *tconn, fr_connection_t *conn, void *uctx);

/** Demultiplex on or more responses, reading them from a connection, decoding them, and matching them with their requests
 *
 * This callback should either:
 *
 * - If an underlying I/O library is used, request complete responses from
 *   the I/O library, and match the responses with a treq (trunk request)
 *   using a tracking structure associated with the #fr_connection_t or uctx.
 * - If no underlying I/O library is used, read responses from the #fr_connection_t,
 *   decode those responses, and match those responses with a treq using a tracking
 *   structure associated with the #fr_connection_t or uctx.
 *
 * The result (positive or negative), should be written to the rctx structure.
 *
 * #fr_trunk_request_signal_complete should be used to inform the trunk
 * that the request is now complete.
 *
 * If a connection appears to have become unusable, this callback should call
 * #fr_connection_signal_reconnect and immediately return.  The current
 * treq will either fail, or be re-enqueued depending on the trunk configuration.
 *
 * #fr_trunk_request_signal_fail should *NOT* be called as this function is only
 * used for reporting failures at an I/O layer level not failures of queries or
 * external services.
 *
 * @param[in] tconn		The trunk connection.
 * @param[in] conn		Connection to read the request from.
 *				Use #fr_connection_get_handle to access the
 *				connection handle or file descriptor.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_request_demux_t)(fr_trunk_connection_t *tconn, fr_connection_t *conn, void *uctx);

/** Inform a remote service like a datastore that a request should be cancelled
 *
 * This callback will be called any time there are one or more requests to be
 * cancelled and a #fr_connection_t is writable, or as soon as a request is
 * cancelled if `always_writable == true`.
 *
 * For efficiency, this callback should call #fr_trunk_connection_pop_cancellation
 * multiple times, and process all outstanding cancellation requests.
 *
 * If the response (cancel ACK) from the remote service needs to be tracked,
 * then the treq should be inserted into a tracking tree shared with the demuxer,
 * and #fr_trunk_request_signal_cancel_sent should be called to move the treq into
 * the cancel_sent state.
 *
 * As with the main mux callback, if a cancellation request is partially written
 * #fr_trunk_request_signal_cancel_partial should be called, and the amount
 * of data written should be tracked in the preq (protocol request).
 *
 * When the demuxer finds a matching (cancel ACK) response, the demuxer should
 * remove the entry from the tracking tree and call
 * #fr_trunk_request_signal_cancel_complete.
 *
 * @param[in] tconn		The trunk connection used to dequeue
 *				cancellation requests.
 * @param[in] conn		Connection to write the request to.
 *				Use #fr_connection_get_handle to access the
 *				connection handle or file descriptor.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_request_cancel_mux_t)(fr_trunk_connection_t *tconn, fr_connection_t *conn, void *uctx);

/** Remove an outstanding request from a tracking/matching structure
 *
 * The treq (trunk request), and any associated resources should be
 * removed from the the matching structure associated with the #fr_connection_t or uctx.
 *
 * Which resources should be freed depends on the cancellation reason:
 *
 * - FR_TRUNK_CANCEL_REASON_MOVE - If an encoded request can be reused
 *   it should be kept.  The trunk mux callback should be aware that
 *   an encoded request may already be associated with a preq and use
 *   that instead of re-encoding the preq.
 *   If the encoded request cannot be reused it should be freed, and
 *   any fields in the preq that were modified during the last mux call
 *   (other than perhaps counters) should be reset to their initial values.
 * - FR_TRUNK_CANCEL_REASON_SIGNAL - The encoded request and any I/O library
 *   request handled may be freed or that may be left to another callback.
 *
 * After this callback is complete one of several actions will be taken:
 *
 * - If the cancellation reason was FR_TRUNK_CANCEL_REASON_MOVE, the treq
 *   will move to the unassigned state, and then either be placed in the
 *   trunk backlog, or immediately enqueued on another trunk connection.
 * - If the reason was FR_TRUNK_CANCEL_SIGNAL
 *   - ...and a request_cancel_mux callback was provided, the
 *     the request_cancel_mux callback will be called when the connection
 *     is next writable (or immediately if `always_writable == true`) and
 *     the request_cancel_mux callback will send an explicit cancellation
 *     request to terminate any outstanding queries on remote datastores.
 *   - ...and no request_cancel_mux callback was provided, the
 *     treq will enter the unassigned state and then be freed.
 *
 * @note FR_TRUNK_CANCEL_REASON_MOVE will only be set if the underlying connection
 * is bad.  No cancellation requests will be sent for requests being moved.
 *
 * @param[in] conn		to remove request from.
 * @param[in] treq		Trunk request to cancel.
 * @param[in] preq		Preq to cancel.
 * @param[in] reason		Why the request was cancelled.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_request_cancel_t)(fr_connection_t *conn, fr_trunk_request_t *treq, void *preq,
					  fr_trunk_cancel_reason_t reason, void *uctx);

/** Write a successful result to the rctx so that the trunk API client is aware of the result
 *
 * The rctx should be modified in such a way that indicates to the trunk API client
 * that the request was sent using the trunk and a response was received.
 *
 * This callback should free any memory not bound to the lifetime of the rctx
 * or request, or that was allocated explicitly to prepare for the REQUEST *
 * being used by a trunk.  This may include I/O library request handles, raw
 * responses, and decoded responses.
 *
 * After this callback is complete, the request_free callback will be called if provided.
 */
typedef void (*fr_trunk_request_complete_t)(REQUEST *request, void *preq, void *rctx, void *uctx);

/** Write a failure result to the rctx so that the trunk API client is aware that the request failed
 *
 * The rctx should be modified in such a way that indicates to the trunk API client
 * that the request could not be sent using the trunk.
 *
 * This callback should free any memory not bound to the lifetime of the rctx
 * or request, or that was allocated explicitly to prepare for the REQUEST *
 * being used by a trunk.
 *
 * @note If a cancel function is provided, the cancel function should be used to remove
 *       active requests from any request/response matching, not the fail function.
 *	 Both the cancel and fail functions will be called for a request that has been
 *	 sent or partially sent.
 *
 * After this callback is complete, the request_free callback will be called if provided.
 */
typedef void (*fr_trunk_request_fail_t)(REQUEST *request, void *preq, void *rctx, void *uctx);

/** Free resources associated with a trunk request
 *
 * The trunk request is complete.  If there's a request still associated with the
 * trunk request, that will be provided so that it can be marked runnable, but
 * be aware that the REQUEST * value will be NULL if the request was cancelled due
 * to a signal.
 *
 * The preq and any associated data such as encoded packets or I/O library request
 * handled *SHOULD* be explicitly freed by this function.
 * The exception to this is if the preq is parented by the treq, in which case the
 * preq will be explicitly freed when the treq is returned to the free list.
 *
 * @param[in] request		to mark as runnable if no further processing is required.
 * @param[in] preq_to_free	As per the name.
 * @param[in] uctx		User context data passed to #fr_trunk_alloc.
 */
typedef void (*fr_trunk_request_free_t)(REQUEST *request, void *preq_to_free, void *uctx);

/** I/O functions to pass to fr_trunk_alloc
 *
 */
typedef struct {
	fr_trunk_connection_alloc_t	connection_alloc;	//!< Allocate a new fr_connection_t.

	fr_trunk_connection_notify_t	connection_notify;	//!< Update the I/O event registrations for

	fr_heap_cmp_t			connection_prioritise;	//!< Ordering function for connections.

	fr_heap_cmp_t			request_prioritise;	//!< Ordering function for requests.  Controls
								///< where in the outbound queues they're inserted.

	fr_trunk_request_mux_t		request_mux;		///!< Write one or more requests to a connection.

	fr_trunk_request_demux_t	request_demux;		///!< Read one or more requests from a connection.

	fr_trunk_request_cancel_mux_t	request_cancel_mux;	//!< Inform an external resource that we no longer
								///< care about the result of any queries we
								///< issued for this request.

	fr_trunk_request_cancel_t	request_cancel;		//!< Request should be removed from tracking
								///< and should be reset to its initial state.

	fr_trunk_request_complete_t	request_complete;	//!< Request is complete.

	fr_trunk_request_fail_t		request_fail;		//!< Cleanup all resources, and inform the caller.

	fr_trunk_request_free_t		request_free;		//!< Free the preq and provide a chance
								///< to mark the request as runnable.
} fr_trunk_io_funcs_t;

/** @name Statistics
 * @{
 */
uint16_t	fr_trunk_connection_count_by_state(fr_trunk_t *trunk, int conn_state);

uint32_t	fr_trunk_request_count_by_connection(fr_trunk_connection_t const *tconn, int req_state);

uint64_t	fr_trunk_request_count_by_state(fr_trunk_t *trunk, int conn_state, int req_state);
/** @} */

/** @name Request state signalling
 * @{
 */
void		fr_trunk_request_signal_partial(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_sent(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_complete(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_fail(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_cancel(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_cancel_partial(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_cancel_sent(fr_trunk_request_t *treq);

void		fr_trunk_request_signal_cancel_complete(fr_trunk_request_t *treq);
/** @} */

/** @name (R)enqueue and alloc requests
 * @{
 */
uint64_t 	fr_trunk_connection_requests_requeue(fr_trunk_connection_t *tconn, int states, uint64_t max);

void		fr_trunk_request_free(fr_trunk_request_t *treq);

fr_trunk_request_t *fr_trunk_request_alloc(fr_trunk_t *trunk, REQUEST *request);

void		fr_trunk_request_requeue(fr_trunk_request_t *treq);

int		fr_trunk_request_enqueue(fr_trunk_request_t **treq, fr_trunk_t *trunk, REQUEST *request,
					 void *preq, void *rctx);
/** @} */

/** @name Dequeue protocol requests and cancellations
 * @{
 */
fr_trunk_request_t *fr_trunk_connection_pop_cancellation(void **preq, fr_trunk_connection_t *tconn);

fr_trunk_request_t *fr_trunk_connection_pop_request(REQUEST **request, void **preq, void **rctx,
						    fr_trunk_connection_t *tconn);
/** @} */

/** @name Connection state signalling
 *
 * The following states are signalled from I/O event handlers:
 *
 * - writable - The connection is writable (the muxer will be called).
 * - readable - The connection is readable (the demuxer will be called).
 * - reconnect - The connection is likely bad and should be reconnected.
 *   If the code signalling has access to the conn, fr_connection_signal_reconnect
 *   can be used instead of fr_trunk_connection_signal_reconnect.
 *
 * The following states are signalled to control whether a connection may be
 * assigned new requests:
 *
 * - inactive - The connection cannot accept any new requests.  Either due to
 *   congestion or some other administrative reason.
 * - active - The connection can, once again, accept new requests.
 *
 * Note: In normal operation a connection will automatically transition between
 * the active and inactive states if conf->max_req_per_conn is specified and the
 * number of pending requests on that connection are equal to that number.
 * If however, the connection has previously been signalled inactive, it will not
 * automatically be reactivated once the number of requests drops below
 * max_req_per_conn.
 *
 * For other connection states the trunk API should not be signalled directly.
 * It will be informed by "watch" callbacks inserted into the #fr_connection_t as
 * to when the connection changes state.
 *
 * #fr_trunk_connection_signal_active does not need to be called in any of the
 * #fr_connection_t state callbacks.  It is only used to activate a connection
 * which has been previously marked inactive using
 * #fr_trunk_connection_signal_inactive.
 *
 * If #fr_trunk_connection_signal_inactive is being used to remove a congested
 * connection from the active list (i.e. on receipt of an explicit protocol level
 * congestion notification), consider calling #fr_trunk_connection_requests_requeue
 * with the FR_TRUNK_REQUEST_PENDING state to redistribute that connection's
 * backlog to other connections in the trunk.
 *
 * @{
 */
void		fr_trunk_connection_signal_writable(fr_trunk_connection_t *tconn);

void		fr_trunk_connection_signal_readable(fr_trunk_connection_t *tconn);

void		fr_trunk_connection_signal_inactive(fr_trunk_connection_t *tconn);

void		fr_trunk_connection_signal_active(fr_trunk_connection_t *tconn);

void		fr_trunk_connection_signal_reconnect(fr_trunk_connection_t *tconn, fr_connection_reason_t reason);
/** @} */

/** @name Connection management
 * @{
 */
void		fr_trunk_reconnect(fr_trunk_t *trunk, int state, fr_connection_reason_t reason);
/** @} */

/** @name Trunk allocation
 * @{
 */
int		fr_trunk_start(fr_trunk_t *trunk);

void		fr_trunk_connection_manage_start(fr_trunk_t *trunk);

void		fr_trunk_connection_manage_stop(fr_trunk_t *trunk);

fr_trunk_t	*fr_trunk_alloc(TALLOC_CTX *ctx, fr_event_list_t *el,
				fr_trunk_io_funcs_t const *funcs, fr_trunk_conf_t const *conf,
				char const *log_prefix, void const *uctx, bool delay_start);
/** @} */

#ifdef __cplusplus
}
#endif
