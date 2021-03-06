// Copyright (c) 2019-2020, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include "zmq.hpp"
#include <string>
#include <list>
#include <queue>
#include <unordered_map>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>
#include <chrono>
#include <atomic>
#include "bt_serialize.h"
#include "string_view.h"

namespace lokimq {

using namespace std::literals;

/// Logging levels passed into LogFunc
enum class LogLevel { trace, debug, info, warn, error, fatal };

/// Authentication levels for command categories and connections
enum class AuthLevel {
    denied, ///< Not actually an auth level, but can be returned by the AllowFunc to deny an incoming connection.
    none, ///< No authentication at all; any random incoming ZMQ connection can invoke this command.
    basic, ///< Basic authentication commands require a login, or a node that is specifically configured to be a public node (e.g. for public RPC).
    admin, ///< Advanced authentication commands require an admin user, either via explicit login or by implicit login from localhost.  This typically protects administrative commands like shutting down, starting mining, or access sensitive data.
};

/// The access level for a command category
struct Access {
    /// Minimum access level required
    AuthLevel auth = AuthLevel::none;
    /// If true only remote SNs may call the category commands
    bool remote_sn = false;
    /// If true the category requires that the local node is a SN
    bool local_sn = false;
};

/// Return type of the AllowFunc: this determines whether we allow the connection at all, and if
/// so, sets the initial authentication level and tells LokiMQ whether the other hand is an
/// active SN.
struct Allow {
    AuthLevel auth = AuthLevel::none;
    bool remote_sn = false;
};

class LokiMQ;

/// Encapsulates an incoming message from a remote connection with message details plus extra
/// info need to send a reply back through the proxy thread via the `reply()` method.  Note that
/// this object gets reused: callbacks should use but not store any reference beyond the callback.
class Message {
public:
    LokiMQ& lokimq; ///< The owning LokiMQ object
    std::vector<string_view> data; ///< The provided command data parts, if any.
    string_view pubkey; ///< The originator pubkey (32 bytes)
    bool service_node; ///< True if the pubkey is an active SN (note that this is only checked on initial connection, not every received message)

    /// Constructor
    Message(LokiMQ& lmq) : lokimq{lmq} {}

    // Non-copyable
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    /// Sends a reply.  Arguments are forwarded to send() but with send_option::optional{} added
    /// if the originator is not a SN.  For SN messages (i.e.  where `sn` is true) this is a
    /// "strong" reply by default in that the proxy will attempt to establish a new connection
    /// to the SN if no longer connected.  For non-SN messages the reply will be attempted using
    /// the available routing information, but if the connection has already been closed the
    /// reply will be dropped.
    ///
    /// If you want to send a non-strong reply even when the remote is a service node then add
    /// an explicit `send_option::optional()` argument.
    template <typename... Args>
    void reply(const std::string& command, Args&&... args);
};


/** The keep-alive time for a send() that results in a establishing a new outbound connection.  To
 * use a longer keep-alive to a host call `connect()` first with the desired keep-alive time or pass
 * the send_option::keep_alive.
 */
static constexpr auto DEFAULT_SEND_KEEP_ALIVE = 30s;

/// Maximum length of a category
static constexpr size_t MAX_CATEGORY_LENGTH = 50;

/// Maximum length of a command
static constexpr size_t MAX_COMMAND_LENGTH = 200;

/**
 * Class that handles LokiMQ listeners, connections, proxying, and workers.  An application
 * typically has just one instance of this class.
 */
class LokiMQ {

private:

    /// The global context
    zmq::context_t context;

    /// A unique id for this LokiMQ instance, assigned in a thread-safe manner during construction.
    const int object_id;

    /// The x25519 keypair of this connection.  For service nodes these are the long-run x25519 keys
    /// provided at construction, for non-service-node connections these are generated during
    /// construction.
    std::string pubkey, privkey;

    /// True if *this* node is running in service node mode (whether or not actually active)
    bool local_service_node = false;

    /// Addresses on which to listen, or empty if we only establish outgoing connections but aren't
    /// listening.
    std::vector<std::string> bind;

    /// The thread in which most of the intermediate work happens (handling external connections
    /// and proxying requests between them to worker threads)
    std::thread proxy_thread;

    /// Will be true (and is guarded by a mutex) if the proxy thread is quitting; guards against new
    /// control sockets from threads trying to talk to the proxy thread.
    bool proxy_shutting_down = false;

    /// Called to obtain a "command" socket that attaches to `control` to send commands to the
    /// proxy thread from other threads.  This socket is unique per thread and LokiMQ instance.
    zmq::socket_t& get_control_socket();

    /// Stores all of the sockets created in different threads via `get_control_socket`.  This is
    /// only used during destruction to close all of those open sockets, and is protected by an
    /// internal mutex which is only locked by new threads getting a control socket and the
    /// destructor.
    std::vector<std::shared_ptr<zmq::socket_t>> thread_control_sockets;

public:

    /// Callback type invoked to determine whether the given new incoming connection is allowed to
    /// connect to us and to set its initial authentication level.
    ///
    /// @param ip - the ip address of the incoming connection
    /// @param pubkey - the x25519 pubkey of the connecting client (32 byte string)
    ///
    /// @returns an `AuthLevel` enum value indicating the default auth level for the incoming
    /// connection, or AuthLevel::denied if the connection should be refused.
    using AllowFunc = std::function<Allow(string_view ip, string_view pubkey)>;

    /// Callback that is invoked when we need to send a "strong" message to a SN that we aren't
    /// already connected to and need to establish a connection.  This callback returns the ZMQ
    /// connection string we should use which is typically a string such as `tcp://1.2.3.4:5678`.
    using SNRemoteAddress = std::function<std::string(const std::string& pubkey)>;

    /// The callback type for registered commands.
    using CommandCallback = std::function<void(Message& message)>;

    /// Called to write a log message.  This will only be called if the `level` is >= the current
    /// LokiMQ object log level.  It must be a raw function pointer (or a capture-less lambda) for
    /// performance reasons.  Takes four arguments: the log level of the message, the filename and
    /// line number where the log message was invoked, and the log message itself.
    using Logger = std::function<void(LogLevel level, const char* file, int line, std::string msg)>;

    /// Explicitly non-copyable, non-movable because most things here aren't copyable, and a few
    /// things aren't movable, either.  If you need to pass the LokiMQ instance around, wrap it
    /// in a unique_ptr or shared_ptr.
    LokiMQ(const LokiMQ&) = delete;
    LokiMQ& operator=(const LokiMQ&) = delete;
    LokiMQ(LokiMQ&&) = delete;
    LokiMQ& operator=(LokiMQ&&) = delete;

    /** How long to wait for handshaking to complete on external connections before timing out and
     * closing the connection.  Setting this only affects new outgoing connections. */
    std::chrono::milliseconds SN_HANDSHAKE_TIME = 10s;

    /** Maximum incoming message size; if a remote tries sending a message larger than this they get
     * disconnected. -1 means no limit. */
    int64_t SN_ZMQ_MAX_MSG_SIZE = 1 * 1024 * 1024;

    /** How long (in ms) to linger sockets when closing them; this is the maximum time zmq spends
     * trying to sending pending messages before dropping them and closing the underlying socket
     * after the high-level zmq socket is closed. */
    std::chrono::milliseconds CLOSE_LINGER = 5s;

private:

    /// The lookup function that tells us where to connect to a peer
    SNRemoteAddress peer_lookup;

    /// Callback to see whether the incoming connection is allowed
    AllowFunc allow_connection;

    /// The log level; this is atomic but we use relaxed order to set and access it (so changing it
    /// might not be instantly visible on all threads, but that's okay).
    std::atomic<LogLevel> log_lvl;

    /// The callback to call with log messages
    Logger logger;

    /// Logging implementation
    template <typename... T>
    void log_(LogLevel lvl, const char* filename, int line, const T&... stuff);

    ///////////////////////////////////////////////////////////////////////////////////
    /// NB: The following are all the domain of the proxy thread (once it is started)!

    /// Addresses to bind to in `start()`
    std::vector<std::string> bind_addresses;

    /// Our listening ROUTER socket for incoming connections (will be left unconnected if not
    /// listening).
    zmq::socket_t listener;

    /// Info about a peer's established connection to us.  Note that "established" means both
    /// connected and authenticated.
    struct peer_info {
        /// True if we've authenticated this peer as a service node.
        bool service_node = false;

        /// The auth level of this peer
        AuthLevel auth_level = AuthLevel::none;

        /// Will be set to a non-empty routing prefix if if we have (or at least recently had) an
        /// established incoming connection with this peer.  Will be empty if there is no incoming
        /// connection.
        std::string incoming;

        /// The index in `remotes` if we have an established outgoing connection to this peer, -1 if
        /// we have no outgoing connection to this peer.
        int outgoing = -1;

        /// The last time we sent or received a message (or had some other relevant activity) with
        /// this peer.  Used for closing outgoing connections that have reached an inactivity expiry
        /// time.
        std::chrono::steady_clock::time_point last_activity;

        /// Updates last_activity to the current time
        void activity() { last_activity = std::chrono::steady_clock::now(); }

        /// After more than this much inactivity we will close an idle connection
        std::chrono::milliseconds idle_expiry;
    };

    struct pk_hash {
        size_t operator()(const std::string& pubkey) const {
            size_t h;
            std::memcpy(&h, pubkey.data(), sizeof(h));
            return h;
        }
    };
    /// Currently peer connections, pubkey -> peer_info
    std::unordered_map<std::string, peer_info, pk_hash> peers;

    /// different polling sockets the proxy handler polls: this always contains some internal
    /// sockets for inter-thread communication followed by listener socket and a pollitem for every
    /// (outgoing) remote socket in `remotes`.  This must be in a sequential vector because of zmq
    /// requirements (otherwise it would be far nicer to not have to synchronize the two vectors).
    std::vector<zmq::pollitem_t> pollitems;

    /// Properly adds a socket to poll for input to pollitems
    void add_pollitem(zmq::socket_t& sock);

    /// The number of internal sockets in `pollitems`
    static constexpr size_t poll_internal_size = 3;

    /// The pollitems location corresponding to `remotes[0]`.
    const size_t poll_remote_offset; // will be poll_internal_size + 1 for a full listener (the +1 is the listening socket); poll_internal_size for a remote-only

    /// The outgoing remote connections we currently have open along with the remote pubkeys.  Each
    /// element [i] here corresponds to an the pollitem_t at pollitems[i+1+poll_internal_size].
    /// (Ideally we'd use one structure, but zmq requires the pollitems be in contiguous storage).
    std::vector<std::pair<std::string, zmq::socket_t>> remotes;

    /// Socket we listen on to receive control messages in the proxy thread. Each thread has its own
    /// internal "control" connection (returned by `get_control_socket()`) to this socket used to
    /// give instructions to the proxy such as instructing it to initiate a connection to a remote
    /// or send a message.
    zmq::socket_t command{context, zmq::socket_type::router};

    /// Router socket to reach internal worker threads from proxy
    zmq::socket_t workers_socket{context, zmq::socket_type::router};

    /// indices of idle, active workers
    std::vector<unsigned int> idle_workers;

    /// Maximum number of general task workers, specified during construction
    unsigned int general_workers;

    /// Maximum number of possible worker threads we can have.  This is calculated when starting,
    /// and equals general_workers plus the sum of all categories' reserved threads counts.  This is
    /// also used to signal a shutdown; we set it to 0 when quitting.
    unsigned int max_workers;

    /// Worker thread loop
    void worker_thread(unsigned int index);

    /// Does the proxying work
    void proxy_loop();

    /// Handles built-in primitive commands in the proxy thread for things like "BYE" that have to
    /// be done in the proxy thread anyway (if we forwarded to a worker the worker would just have
    /// to send an instruction back to the proxy to do it).  Returns true if one was handled, false
    /// to continue with sending to a worker.
    bool proxy_handle_builtin(size_t conn_index, std::vector<zmq::message_t>& parts);

    /// Sets up a job for a worker then signals the worker (or starts a worker thread)
    void proxy_to_worker(size_t conn_index, std::vector<zmq::message_t>& parts);

    /// proxy thread command handlers for commands sent from the outer object QUIT.  This doesn't
    /// get called immediately on a QUIT command: the QUIT commands tells workers to quit, then this
    /// gets called after all works have done so.
    void proxy_quit();

    /// Common connection implementation used by proxy_connect/proxy_send.  Returns the socket
    /// and, if a routing prefix is needed, the required prefix (or an empty string if not needed).
    /// For an optional connect that fail, returns nullptr for the socket.
    std::pair<zmq::socket_t*, std::string> proxy_connect(const std::string& pubkey, const std::string& connect_hint, bool optional, bool incoming_only, std::chrono::milliseconds keep_alive);

    /// CONNECT command telling us to connect to a new pubkey.  Returns the socket (which could be
    /// existing or a new one).
    std::pair<zmq::socket_t*, std::string> proxy_connect(bt_dict&& data);

    /// DISCONNECT command telling us to disconnect our remote connection to the given pubkey (if we
    /// have one).
    void proxy_disconnect(const std::string& pubkey);

    /// SEND command.  Does a connect first, if necessary.
    void proxy_send(bt_dict&& data);

    /// REPLY command.  Like SEND, but only has a listening socket route to send back to and so is
    /// weaker (i.e. it cannot reconnect to the SN if the connection is no longer open).
    void proxy_reply(bt_dict&& data);

    /// ZAP (https://rfc.zeromq.org/spec:27/ZAP/) authentication handler; this is called with the
    /// zap auth socket to do non-blocking processing of any waiting authentication requests waiting
    /// on it to verify whether the connection is from a valid/allowed SN.
    void process_zap_requests(zmq::socket_t& zap_auth);

    /// Handles a control message from some outer thread to the proxy
    void proxy_control_message(std::vector<zmq::message_t> parts);

    /// Closing any idle connections that have outlived their idle time.  Note that this only
    /// affects outgoing connections; incomings connections are the responsibility of the other end.
    void proxy_expire_idle_peers();

    /// Closes an outgoing connection immediately, updates internal variables appropriately.
    /// Returns the next iterator (the original may or may not be removed from peers, depending on
    /// whether or not it also has an active incoming connection).
    decltype(peers)::iterator proxy_close_outgoing(decltype(peers)::iterator it);

    struct category {
        Access access;
        std::unordered_map<std::string, CommandCallback> commands;
        unsigned int reserved_threads = 0;
        unsigned int active_threads = 0;
        std::queue<std::list<zmq::message_t>> pending; // FIXME - vector?
        int max_queue = 200;

        category(Access access, unsigned int reserved_threads, int max_queue)
            : access{access}, reserved_threads{reserved_threads}, max_queue{max_queue} {}
    };

    /// Categories, mapped by category name.
    std::unordered_map<std::string, category> categories;

    /// For enabling backwards compatibility with command renaming: this allows mapping one command
    /// to another in a different category (which happens before the category and command lookup is
    /// done).
    std::unordered_map<std::string, std::string> command_aliases;

    /// Retrieve category and callback from a command name, including alias mapping.  Warns on
    /// invalid commands and returns nullptrs.  The command name will be updated in place if it is
    /// aliased to another command.
    std::pair<const category*, const CommandCallback*> get_command(std::string& command);

    /// Checks a peer's authentication level.  Returns true if allowed, warns and returns false if
    /// not.
    bool proxy_check_auth(const std::string& pubkey, size_t conn_index, const peer_info& peer,
            const std::string& command, const category& cat, zmq::message_t& msg);

    /// 


    /// End of proxy-specific members
    ///////////////////////////////////////////////////////////////////////////////////


    /// Structure that contains the data for a worker thread - both the thread itself, plus any
    /// transient data we are passing into the thread.
    struct run_info {
        std::string command;
        std::string pubkey;
        bool service_node = false;
        const CommandCallback* callback = nullptr;
        std::vector<zmq::message_t> message_parts;

    private:
        friend class LokiMQ;
        std::thread thread;
        std::string routing_id;
    };
    /// Data passed to workers for the RUN command.  The proxy thread sets elements in this before
    /// sending RUN to a worker then the worker uses it to get call info, and only allocates it
    /// once, before starting any workers.  Workers may only access their own index and may not
    /// change it.
    std::vector<run_info> workers;

public:
    /**
     * LokiMQ constructor.  This constructs the object but does not start it; you will typically
     * want to first add categories and commands, then finish startup by invoking `start()`.
     * (Categories and commands cannot be added after startup).
     *
     * @param pubkey the public key (32-byte binary string).  For a service node this is the service
     * node x25519 keypair.  For non-service nodes this (and privkey) can be empty strings to
     * automatically generate an ephemeral keypair.
     *
     * @param privkey the service node's private key (32-byte binary string), or empty to generate
     * one.
     *
     * @param service_node - true if this instance should be considered a service node for the
     * purpose of allowing "Access::local_sn" remote calls.  (This should be true if we are
     * *capable* of being a service node, whether or not we are currently actively).  If specified
     * as true then the pubkey and privkey values must not be empty.
     *
     * @param bind list of addresses to bind to.  Can be any string zmq supports; typically a tcp
     * IP/port combination such as: "tcp://\*:4567" or "tcp://1.2.3.4:5678".  Can be empty to not
     * listen at all.
     *
     * @param peer_lookup function that takes a pubkey key (32-byte binary string) and returns a
     * connection string such as "tcp://1.2.3.4:23456" to which a connection should be established
     * to reach that service node.  Note that this function is only called if there is no existing
     * connection to that service node, and that the function is never called for a connection to
     * self (that uses an internal connection instead).
     *
     * @param allow_incoming is a callback that LokiMQ can use to determine whether an incoming
     * connection should be allowed at all and, if so, whether the connection is from a known
     * service node.  Called with the connecting IP, the remote's verified x25519 pubkey, and the 
     * called on incoming connections with the (verified) incoming connection
     * pubkey (32-byte binary string) to determine whether the given SN should be allowed to
     * connect.
     *
     * @param log a function or callable object that writes a log message.  If omitted then all log
     * messages are suppressed.
     *
     * @param general_workers the maximum number of worker threads to start for general tasks.
     * These threads can be used for any command, and will be created (up to the limit) on demand.
     * Note that individual categories with reserved threads can create threads in addition to the
     * amount specified here.  The default (0) means std::thread::hardware_concurrency().
     */
    LokiMQ( std::string pubkey,
            std::string privkey,
            bool service_node,
            std::vector<std::string> bind,
            SNRemoteAddress peer_lookup,
            AllowFunc allow_connection,
            Logger logger = [](LogLevel, const char*, int, std::string) { },
            unsigned int general_workers = 0);

    /**
     * Destructor; instructs the proxy to quit.  The proxy tells all workers to quit, waits for them
     * to quit and rejoins the threads then quits itself.  The outer thread (where the destructor is
     * running) rejoins the proxy thread.
     */
    ~LokiMQ();

    /// Sets the log level of the LokiMQ object.
    void log_level(LogLevel level);

    /// Gets the log level of the LokiMQ object.
    LogLevel log_level() const;

    /**
     * Add a new command category.  This method may not be invoked after `start()` has been called.
     * This method is also not thread safe, and is generally intended to be called (along with
     * add_command) immediately after construction and immediately before calling start().
     *
     * @param name - the category name which must consist of one or more characters and may not
     * contain a ".".
     *
     * @param access_level the access requirements for remote invocation of the commands inside this
     * category.
     *
     * @param reserved_threads if non-zero then the worker thread pool will ensure there are at at
     * least this many threads either current processing or available to process commands in this
     * category.  This is used to ensure that a category's commands can be invoked even if
     * long-running commands in some other category are currently using all worker threads.  This
     * can increase the number of worker threads above the `general_workers` parameter given in the
     * constructor, but will only do so if the need arised: that is, if a command request arrives
     * for a category when all workers are busy and no worker is currently processing any command in
     * that category.
     *
     * @param max_queue is the maximum number of incoming messages in this category that we will
     * queue up when waiting for a worker to become available for this category.  Once the queue
     * for a category exceeds this many incoming messages then new messages will be dropped until
     * some messages are processed off the queue.  -1 means unlimited, 0 means we will just drop
     * messages for this category when no workers are available.
     */
    void add_category(std::string name, Access access_level, unsigned int reserved_threads = 0, int max_queue = 200);

    /**
     * Adds a new command to an existing category.  This method may not be invoked after `start()`
     * has been called.
     *
     * @param category - the category name (must already be created by a call to `add_category`)
     *
     * @param name - the command name, without the `category.` prefix.
     *
     * @param callback - a callable object which is callable as `callback(zeromq::Message &)`
     */
    void add_command(const std::string& category, std::string name, CommandCallback callback);

    /**
     * Adds a command alias; this is intended for temporary backwards compatibility: if any aliases
     * are defined then every command (not just aliased ones) has to be checked on invocation to see
     * if it is defined in the alias list.  May not be invoked after `start()`.
     *
     * Aliases should follow the `category.command` format for both the from and to names, and
     * should only be called for `to` categories that are already defined.  The category name is not
     * currently enforced on the `from` name (for backwards compatility with Loki's quorumnet code)
     * but will be at some point.
     *
     * Access permissions for an aliased command depend only on the mapped-to value; for example, if
     * `cat.meow` is aliased to `dog.bark` then it is the access permissions on `dog` that apply,
     * not those of `cat`, even if `cat` is more restrictive than `dog`.
     */
    void add_command_alias(std::string from, std::string to);

    /**
     * Finish starting up: binds to the bind locations given in the constructor and launches the
     * proxy thread to handle message dispatching between remote nodes and worker threads.
     *
     * You will need to call `add_category` and `add_command` to register commands before calling
     * `start()`; once start() is called commands cannot be changed.
     */
    void start();

    /**
     * Try to initiate a connection to the given SN in anticipation of needing a connection in the
     * future.  If a connection is already established, the connection's idle timer will be reset
     * (so that the connection will not be closed too soon).  If the given idle timeout is greater
     * than the current idle timeout then the timeout increases to the new value; if less than the
     * current timeout it is ignored.  (Note that idle timeouts only apply if the existing
     * connection is an outgoing connection).
     *
     * Note that this method (along with send) doesn't block waiting for a connection; it merely
     * instructs the proxy thread that it should establish a connection.
     *
     * @param pubkey - the public key (32-byte binary string) of the service node to connect to
     * @param keep_alive - the connection will be kept alive if there was valid activity within
     *                     the past `keep_alive` milliseconds.  If an outgoing connection already
     *                     exists, the longer of the existing and the given keep alive is used.
     *                     Note that the default applied here is much longer than the default for an
     *                     implicit connect() by calling send() directly.
     * @param hint - if non-empty and a new outgoing connection needs to be made this hint value
     *               may be used instead of calling the lookup function.  (Note that there is no
     *               guarantee that the hint will be used; it is only usefully specified if the
     *               connection location has already been incidentally determined).
     */
    void connect(const std::string& pubkey, std::chrono::milliseconds keep_alive = 5min, const std::string& hint = "");

    /**
     * Queue a message to be relayed to the SN identified with the given pubkey without expecting a
     * reply.  LokiMQ will attempt to relay the message (first connecting and handshaking if not
     * already connected to the given SN).
     *
     * If a new connection it established it will have a relatively short (30s) idle timeout.  If
     * the connection should stay open longer you should call `connect(pubkey, IDLETIME)` first.
     *
     * Note that this method (along with connect) doesn't block waiting for a connection or for the
     * message to send; it merely instructs the proxy thread that it should send.  ZMQ will
     * generally try hard to deliver it (reconnecting if the connection fails), but if the
     * connection fails persistently the message will eventually be dropped.
     *
     * @param pubkey - the pubkey to send this to
     * @param cmd - the first data frame value which is almost always the remote "category.command" name
     * @param opts - any number of std::string and send options.  Each send option affects
     *               how the send works; each string becomes a serialized message part.
     *
     * Example:
     *
     *     lmq.send(pubkey, "hello", "abc", send_option::hint("tcp://localhost:1234"), "def");
     *
     * sends the command `hello` to the given pubkey, containing additional message parts "abc" and
     * "def", and, if not currently connected, using the given connection hint rather than
     * performing a connection address lookup on the pubkey.
     */
    template <typename... T>
    void send(const std::string& pubkey, const std::string& cmd, const T&... opts);

    /**
     * Similar to the above, but takes an iterator pair of message parts to send after the value.
     *
     * @param pubkey - the pubkey to send this to
     * @param cmd - the value of the first message part (i.e. the remote command)
     * @param first - an input iterator to std::string values
     * @param last - the beyond-the-end iterator
     * @param opts - any number of send options.  This may also contain additional message strings
     * which will be appended after the `[first, last)` message parts.
     */
    template <typename InputIt, typename... T>
    void send(const std::string& pubkey, const std::string& cmd, InputIt first, InputIt end, const T&... opts);


    /// The key pair this LokiMQ was created with; if empty keys were given during construction then
    /// this returns the generated keys.
    const std::string& get_pubkey() const { return pubkey; }
    const std::string& get_privkey() const { return privkey; }
};

/// Namespace for options to the send() method
namespace send_option {

/// `serialized` lets you serialize once when sending the same data to many peers by constructing a
/// single serialized option and passing it repeatedly rather than needing to reserialize on each
/// send.
struct serialized {
    std::string data;
    template <typename T>
    serialized(const T& arg) : data{lokimq::bt_serialize(arg)} {}
};

/// Specifies a connection hint when passed in to send().  If there is no current connection to the
/// peer then the hint is used to save a call to the SNRemoteAddress to get the connection location.
/// (Note that there is no guarantee that the given hint will be used or that a SNRemoteAddress call
/// will not also be done.)
struct hint {
    std::string connect_hint;
    hint(std::string connect_hint) : connect_hint{std::move(connect_hint)} {}
};

/// Does a send() if we already have a connection (incoming or outgoing) with the given peer,
/// otherwise drops the message.
struct optional {};

/// Specifies that the message should be sent only if it can be sent on an existing incoming socket,
/// and dropped otherwise.
struct incoming {};

/// Specifies the idle timeout for the connection - if a new or existing outgoing connection is used
/// for the send and its current idle timeout setting is less than this value then it is updated.
struct keep_alive {
    std::chrono::milliseconds time;
    keep_alive(std::chrono::milliseconds time) : time{std::move(time)} {}
};

}

namespace detail {

// Sends a control message to the given socket consisting of the command plus optional dict
// data (only sent if the data is non-empty).
void send_control(zmq::socket_t& sock, string_view cmd, std::string data = {});

/// Base case: takes a serializable value and appends it to the message parts
template <typename T>
void apply_send_option(bt_list& parts, bt_dict&, const T& arg) {
    parts.push_back(lokimq::bt_serialize(arg));
}

/// `serialized` specialization: lets you serialize once when sending the same data to many peers
template <> inline void apply_send_option(bt_list& parts, bt_dict& , const send_option::serialized& serialized) {
    parts.push_back(serialized.data);
}

/// `hint` specialization: sets the hint in the control data
template <> inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::hint& hint) {
    control_data["hint"] = hint.connect_hint;
}

/// `optional` specialization: sets the optional flag in the control data
template <> inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::optional &) {
    control_data["optional"] = 1;
}

/// `incoming` specialization: sets the optional flag in the control data
template <> inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::incoming &) {
    control_data["incoming"] = 1;
}

/// `keep_alive` specialization: increases the outgoing socket idle timeout (if shorter)
template <> inline void apply_send_option(bt_list&, bt_dict& control_data, const send_option::keep_alive& timeout) {
    control_data["keep-alive"] = timeout.time.count();
}

/// Calls apply_send_option on each argument and returns a bt_dict with the command plus data stored
/// in the "send" key plus whatever else is implied by any given option arguments.
template <typename InputIt, typename... T>
bt_dict send_control_data(const std::string& cmd, InputIt begin, InputIt end, const T &...opts) {
    bt_dict control_data;
    bt_list parts{{cmd}};
    parts.insert(parts.end(), std::move(begin), std::move(end));
#ifdef __cpp_fold_expressions
    (detail::apply_send_option(parts, control_data, opts),...);
#else
    (void) std::initializer_list<int>{(detail::apply_send_option(parts, control_data, opts), 0)...};
#endif

    control_data["send"] = std::move(parts);
    return control_data;
}

} // namespace detail

template <typename InputIt, typename... T>
void LokiMQ::send(const std::string& pubkey, const std::string& cmd, InputIt first, InputIt last, const T &...opts) {
    bt_dict control_data = detail::send_control_data(cmd, std::move(first), std::move(last), opts...);
    control_data["pubkey"] = pubkey;
    detail::send_control(get_control_socket(), "SEND", bt_serialize(control_data));
}

template <typename... T>
void LokiMQ::send(const std::string& pubkey, const std::string& cmd, const T &...opts) {
    const std::string* no_it = nullptr;
    send(pubkey, cmd, no_it, no_it, opts...);
}

template <typename... Args>
void Message::reply(const std::string& command, Args&&... args) {
    if (service_node) lokimq.send(pubkey, command, std::forward<Args>(args)...);
    else lokimq.send(pubkey, command, send_option::optional{}, std::forward<Args>(args)...);
}


template <typename... T>
void LokiMQ::log_(LogLevel lvl, const char* file, int line, const T&... stuff) {
    if (lvl < log_level())
        return;

    std::ostringstream os;
#ifdef __cpp_fold_expressions
    os << ... << stuff;
#else
    (void) std::initializer_list<int>{(os << stuff, 0)...};
#endif
    logger(lvl, file, line, os.str());
}

}

// vim:sw=4:et
