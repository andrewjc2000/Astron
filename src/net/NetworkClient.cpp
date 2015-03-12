#include "NetworkClient.h"
#include <stdexcept>
#include <boost/bind.hpp>
#include "core/global.h"
#include "config/ConfigVariable.h"
using namespace std;
using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

NetworkClient::NetworkClient() : m_socket(nullptr), m_secure_socket(nullptr), m_ssl_enabled(false),
    m_async_timer(io_service), m_send_queue()
{
}

NetworkClient::NetworkClient(tcp::socket *socket) : m_socket(socket), m_secure_socket(nullptr),
    m_ssl_enabled(false), m_async_timer(io_service), m_send_queue()
{
    start_receive();
}

NetworkClient::NetworkClient(ssl::stream<tcp::socket>* stream) :
    m_socket(&stream->next_layer()), m_secure_socket(stream), m_ssl_enabled(true),
    m_async_timer(io_service), m_send_queue()
{
    start_receive();
}

NetworkClient::~NetworkClient()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if(m_ssl_enabled) {
        // This also deletes m_socket:
        delete m_secure_socket;
    } else {
        // ONLY delete m_socket if we must do so directly. If it's wrapped in
        // an SSL stream, the stream "owns" the socket.
        delete m_socket;
    }
    delete [] m_data_buf;
    delete [] m_send_buf;
}

void NetworkClient::set_socket(tcp::socket *socket)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if(m_socket) {
        throw std::logic_error("Trying to set a socket of a network client whose socket was already set.");
    }
    m_socket = socket;

    boost::asio::socket_base::keep_alive keepalive(true);
    m_socket->set_option(keepalive);

    boost::asio::ip::tcp::no_delay nodelay(true);
    m_socket->set_option(nodelay);

    start_receive();
}

void NetworkClient::set_socket(ssl::stream<tcp::socket> *stream)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if(m_socket) {
        throw std::logic_error("Trying to set a socket of a network client whose socket was already set.");
    }

    m_ssl_enabled = true;
    m_secure_socket = stream;

    set_socket(&stream->next_layer());
}

void NetworkClient::set_write_timeout(unsigned int timeout)
{
    m_write_timeout = timeout;
}

void NetworkClient::set_write_buffer(uint64_t max_bytes)
{
    m_max_queue_size = max_bytes;
}

void NetworkClient::send_datagram(DatagramHandle dg)
{
    lock_guard<recursive_mutex> lock(m_lock);

    if(m_is_sending)
    {
        m_send_queue.push(dg);
        m_total_queue_size += dg->size();
        if(m_total_queue_size > m_max_queue_size && m_max_queue_size != 0)
        {
            // TODO: "Write buffer exceeded" error.
            send_disconnect();
        }
    }
    else
    {
        m_is_sending = true;
        async_send(dg);
    }
}

bool NetworkClient::is_connected()
{
    lock_guard<recursive_mutex> lock(m_lock);
    return m_socket->is_open();
}

void NetworkClient::start_receive()
{
    try {
        m_remote = m_socket->remote_endpoint();
    } catch(const boost::system::system_error&) {
        // A client might disconnect immediately after connecting.
        // Since we are in the constructor, ignore it. Resolves #122.
        // When the owner of the NetworkClient attempts to send or receive,
        // the error will occur and we'll cleanup then;
    }
    async_receive();
}

void NetworkClient::async_receive()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    try {
        if(m_is_data) { // Read data
            socket_read(m_data_buf, m_data_size, &NetworkClient::receive_data);
        } else { // Read length
            socket_read(m_size_buf, sizeof(dgsize_t), &NetworkClient::receive_size);
        }
    } catch(const boost::system::system_error &err) {
        // An exception happening when trying to initiate a read is a clear
        // indicator that something happened to the connection. Therefore:
        send_disconnect(err.code());
    }
}

void NetworkClient::send_disconnect()
{
    boost::system::error_code ec;
    send_disconnect(ec);
}

void NetworkClient::send_disconnect(const boost::system::error_code &ec)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_local_disconnect = true;
    m_disconnect_error = ec;
    m_socket->close();
}

void NetworkClient::handle_disconnect(const boost::system::error_code &ec)
{
    if(m_disconnect_handled) {
        return;
    }

    m_disconnect_handled = true;

    if(m_local_disconnect) {
        receive_disconnect(m_disconnect_error);
    } else {
        receive_disconnect(ec);
    }
}

void NetworkClient::receive_size(const boost::system::error_code &ec, size_t bytes_transferred)
{
    if(ec) {
        handle_disconnect(ec);
        return;
    }

    if(bytes_transferred != sizeof(m_size_buf)) {
        boost::system::error_code epipe(boost::system::errc::errc_t::broken_pipe, boost::system::system_category());
        handle_disconnect(epipe);
        return;
    }

    dgsize_t old_size = m_data_size;
    // required to disable strict-aliasing optimizations, which can break the code
    dgsize_t* new_size_p = (dgsize_t*)m_size_buf;
    m_data_size = swap_le(*new_size_p);
    if(m_data_size > old_size) {
        delete [] m_data_buf;
        m_data_buf = new uint8_t[m_data_size];
    }
    m_is_data = true;
    async_receive();
}

void NetworkClient::receive_data(const boost::system::error_code &ec, size_t bytes_transferred)
{
    if(ec) {
        handle_disconnect(ec);
        return;
    }

    if(bytes_transferred != m_data_size) {
        boost::system::error_code epipe(boost::system::errc::errc_t::broken_pipe, boost::system::system_category());
        handle_disconnect(epipe);
        return;
    }

    DatagramPtr dg = Datagram::create(m_data_buf, m_data_size); // Datagram makes a copy
    m_is_data = false;
    receive_datagram(dg);
    async_receive();
}

void NetworkClient::async_send(DatagramHandle dg)
{
    lock_guard<recursive_mutex> lock(m_lock);

    size_t buffer_size = sizeof(dgsize_t) + dg->size();
    dgsize_t len = swap_le(dg->size());
    m_send_buf = new uint8_t[buffer_size];
    memcpy(m_send_buf, (uint8_t*)&len, sizeof(dgsize_t));
    memcpy(m_send_buf+sizeof(dgsize_t), dg->get_data(), dg->size());

    try
    {
        socket_write(m_send_buf, buffer_size);
    }
    catch(const boost::system::system_error& err)
    {
        // An exception happening when trying to initiate a send is a clear
        // indicator that something happened to the connection, therefore:
        send_disconnect(err.code());
    }
}

void NetworkClient::send_finished(const boost::system::error_code &ec, size_t /*bytes_transferred*/)
{
    lock_guard<recursive_mutex> lock(m_lock);

    // Cancel the outstanding timeout
    m_async_timer.cancel();

    // Discard the buffer we just used:
    delete [] m_send_buf;
    m_send_buf = nullptr;

    // Check if the write had errors
    if(ec.value() != 0)
    {
        m_is_sending = false;
        send_disconnect(ec);
        return;
    }

    // Check if we have more items in the queue
    if(m_send_queue.size() > 0)
    {
        // Send the next item in the queue
        DatagramHandle dg = m_send_queue.front();
        m_total_queue_size -= dg->size();
        m_send_queue.pop();
        async_send(dg);
        return;
    }

    // Nothing left in the queue to send, lets open up for another write
    m_is_sending = false;
}

void NetworkClient::send_expired(const boost::system::error_code& ec)
{
    // operation_aborted is received if the the timer is cancelled,
    //     ie. if the send completed before it expires, so don't do anything
    if(ec != boost::asio::error::operation_aborted)
    {
        // TODO: "Write operation timed out" error.
        send_disconnect();
    }
}

void NetworkClient::socket_read(uint8_t* buf, size_t length, receive_handler_t callback)
{
    if(m_ssl_enabled) {
        async_read(*m_secure_socket, boost::asio::buffer(buf, length),
                   boost::bind(callback, this,
                               boost::asio::placeholders::error,
                               boost::asio::placeholders::bytes_transferred));
    } else {
        async_read(*m_socket, boost::asio::buffer(buf, length),
                   boost::bind(callback, this,
                               boost::asio::placeholders::error,
                               boost::asio::placeholders::bytes_transferred));
    }
}

void NetworkClient::socket_write(const uint8_t* buf, size_t length)
{
    // Start async timeout, a value of 0 indicates the writes shouldn't timeout (used in debugging)
    if(m_write_timeout > 0)
    {
        m_async_timer.expires_from_now(boost::posix_time::milliseconds(m_write_timeout));
        m_async_timer.async_wait(boost::bind(&NetworkClient::send_expired, this,
                                             boost::asio::placeholders::error));
    }

    // Start async write
    if(m_ssl_enabled)
    {
        async_write(*m_secure_socket, boost::asio::buffer(buf, length),
                    boost::bind(&NetworkClient::send_finished, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
    }
    else
    {
        async_write(*m_socket, boost::asio::buffer(buf, length),
                    boost::bind(&NetworkClient::send_finished, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
    }
}
