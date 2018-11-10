/***************************************************************************
 *
 * Project         _____    __   ____   _      _
 *                (  _  )  /__\ (_  _)_| |_  _| |_
 *                 )(_)(  /(__)\  )( (_   _)(_   _)
 *                (_____)(__)(__)(__)  |_|    |_|
 *
 *
 * Copyright 2018-present, Leonid Stryzhevskyi, <lganzzzo@gmail.com>
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
 *
 ***************************************************************************/

#include "HttpRequestExecutor.hpp"

#include "oatpp/web/protocol/http/outgoing/Request.hpp"
#include "oatpp/web/protocol/http/outgoing/BufferBody.hpp"

#include "oatpp/network/Connection.hpp"
#include "oatpp/core/data/stream/ChunkedBuffer.hpp"
#include "oatpp/core/data/stream/StreamBufferedProxy.hpp"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

namespace oatpp { namespace web { namespace client {
  
std::shared_ptr<HttpRequestExecutor::ConnectionHandle> HttpRequestExecutor::getConnection() {
  auto connection = m_connectionProvider->getConnection();
  if(!connection){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_CONNECT,
                                "[oatpp::web::client::HttpRequestExecutor::getConnection()]: ConnectionProvider failed to provide Connection");
  }
  return std::make_shared<HttpConnectionHandle>(connection);
}

HttpRequestExecutor::Action HttpRequestExecutor::getConnectionAsync(oatpp::async::AbstractCoroutine* parentCoroutine, AsyncConnectionCallback callback) {
  
  class GetConnectionCoroutine : public oatpp::async::CoroutineWithResult<GetConnectionCoroutine, std::shared_ptr<ConnectionHandle>> {
  private:
    std::shared_ptr<oatpp::network::ClientConnectionProvider> m_connectionProvider;
  public:
    
    GetConnectionCoroutine(const std::shared_ptr<oatpp::network::ClientConnectionProvider>& connectionProvider)
      : m_connectionProvider(connectionProvider)
    {}
    
    Action act() override {
      oatpp::network::ClientConnectionProvider::AsyncCallback callback =
      static_cast<oatpp::network::ClientConnectionProvider::AsyncCallback>(&GetConnectionCoroutine::onConnectionReady);
      return m_connectionProvider->getConnectionAsync(this, callback);
    }
    
    Action onConnectionReady(const std::shared_ptr<oatpp::data::stream::IOStream>& connection) {
      return _return(std::make_shared<HttpConnectionHandle>(connection));
    }
    
  };
  
  return parentCoroutine->startCoroutineForResult<GetConnectionCoroutine>(callback, m_connectionProvider);
  
}
  
std::shared_ptr<HttpRequestExecutor::Response>
HttpRequestExecutor::execute(const String& method,
                             const String& path,
                             const std::shared_ptr<Headers>& headers,
                             const std::shared_ptr<Body>& body,
                             const std::shared_ptr<ConnectionHandle>& connectionHandle) {
  
  std::shared_ptr<oatpp::network::ConnectionProvider::IOStream> connection;
  if(connectionHandle) {
    connection = static_cast<HttpConnectionHandle*>(connectionHandle.get())->connection;
  } else {
    connection = m_connectionProvider->getConnection();
  }
  
  if(!connection){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_CONNECT,
                                "[oatpp::web::client::HttpRequestExecutor::execute()]: ConnectionProvider failed to provide Connection");
  }
  
  auto request = oatpp::web::protocol::http::outgoing::Request::createShared(method, path, headers, body);
  request->headers->putIfNotExists(oatpp::web::protocol::http::Header::HOST, m_connectionProvider->getHost());
  request->headers->putIfNotExists(oatpp::web::protocol::http::Header::CONNECTION,
                                   oatpp::web::protocol::http::Header::Value::CONNECTION_KEEP_ALIVE);
  
  auto ioBuffer = oatpp::data::buffer::IOBuffer::createShared();
  
  auto upStream = oatpp::data::stream::OutputStreamBufferedProxy::createShared(connection, ioBuffer);
  request->send(upStream);
  upStream->flush();
  
  auto readCount = connection->read(ioBuffer->getData(), ioBuffer->getSize());
  
  if(readCount == 0) {
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_NO_RESPONSE,
                                "[oatpp::web::client::HttpRequestExecutor::execute()]: No response from server");
  } else if(readCount < 0) {
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_READ_RESPONSE,
                                "[oatpp::web::client::HttpRequestExecutor::execute()]: Failed to read response. Check out the RequestExecutionError::getReadErrorCode() for more information", (v_int32) readCount);
  }
    
  oatpp::parser::ParsingCaret caret((p_char8) ioBuffer->getData(), ioBuffer->getSize());
  auto line = protocol::http::Protocol::parseResponseStartingLine(caret);
  if(!line){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_PARSE_STARTING_LINE,
                                "[oatpp::web::client::HttpRequestExecutor::execute()]: Failed to parse response. Invalid starting line");
  }
  
  oatpp::web::protocol::http::Status error;
  auto responseHeaders = protocol::http::Protocol::parseHeaders(caret, error);
  
  if(error.code != 0){
    throw RequestExecutionError(RequestExecutionError::ERROR_CODE_CANT_PARSE_HEADERS,
                                "[oatpp::web::client::HttpRequestExecutor::execute()]: Failed to parse response. Invalid headers section");
  }
  
  auto bodyStream = oatpp::data::stream::InputStreamBufferedProxy::createShared(connection,
                                                                                ioBuffer,
                                                                                caret.getPosition(),
                                                                                (v_int32) readCount);
  
  return Response::createShared(line->statusCode, line->description, responseHeaders, bodyStream, m_bodyDecoder);
  
}
  
oatpp::async::Action HttpRequestExecutor::executeAsync(oatpp::async::AbstractCoroutine* parentCoroutine,
                                                       AsyncCallback callback,
                                                       const String& method,
                                                       const String& path,
                                                       const std::shared_ptr<Headers>& headers,
                                                       const std::shared_ptr<Body>& body,
                                                       const std::shared_ptr<ConnectionHandle>& connectionHandle) {
  
  class ExecutorCoroutine : public oatpp::async::CoroutineWithResult<ExecutorCoroutine, std::shared_ptr<HttpRequestExecutor::Response>> {
  private:
    std::shared_ptr<oatpp::network::ClientConnectionProvider> m_connectionProvider;
    String m_method;
    String m_path;
    std::shared_ptr<Headers> m_headers;
    std::shared_ptr<Body> m_body;
    std::shared_ptr<const oatpp::web::protocol::http::incoming::BodyDecoder> m_bodyDecoder;
    std::shared_ptr<ConnectionHandle> m_connectionHandle;
  private:
    std::shared_ptr<oatpp::data::stream::IOStream> m_connection;
    std::shared_ptr<oatpp::data::buffer::IOBuffer> m_ioBuffer;
    void* m_bufferPointer;
    os::io::Library::v_size m_bufferBytesLeftToRead;
  public:
    
    ExecutorCoroutine(const std::shared_ptr<oatpp::network::ClientConnectionProvider>& connectionProvider,
                      const String& method,
                      const String& path,
                      const std::shared_ptr<Headers>& headers,
                      const std::shared_ptr<Body>& body,
                      const std::shared_ptr<const oatpp::web::protocol::http::incoming::BodyDecoder>& bodyDecoder,
                      const std::shared_ptr<ConnectionHandle>& connectionHandle)
      : m_connectionProvider(connectionProvider)
      , m_method(method)
      , m_path(path)
      , m_headers(headers)
      , m_body(body)
      , m_bodyDecoder(bodyDecoder)
      , m_connectionHandle(connectionHandle)
    {}
    
    Action act() override {
      if(m_connectionHandle) {
        /* Careful here onConnectionReady() should have only one possibe state */
        /* Because it is called here in synchronous manner */
        return onConnectionReady(static_cast<HttpConnectionHandle*>(m_connectionHandle.get())->connection);
      } else {
        oatpp::network::ClientConnectionProvider::AsyncCallback callback =
        static_cast<oatpp::network::ClientConnectionProvider::AsyncCallback>(&ExecutorCoroutine::onConnectionReady);
        return m_connectionProvider->getConnectionAsync(this, callback);
      }
    }
    
    /* Careful here onConnectionReady() should have only one possibe state */
    /* Because there is a call to it from act() in synchronous manner */
    Action onConnectionReady(const std::shared_ptr<oatpp::data::stream::IOStream>& connection) {
      m_connection = connection;
      auto request = oatpp::web::protocol::http::outgoing::Request::createShared(m_method, m_path, m_headers, m_body);
      request->headers->putIfNotExists(String(Header::HOST, false), m_connectionProvider->getHost());
      request->headers->putIfNotExists(String(Header::CONNECTION, false), String(Header::Value::CONNECTION_KEEP_ALIVE, false));
      m_ioBuffer = oatpp::data::buffer::IOBuffer::createShared();
      auto upStream = oatpp::data::stream::OutputStreamBufferedProxy::createShared(connection, m_ioBuffer);
      m_bufferPointer = m_ioBuffer->getData();
      m_bufferBytesLeftToRead = m_ioBuffer->getSize();
      return request->sendAsync(this, upStream->flushAsync(this, yieldTo(&ExecutorCoroutine::readResponse)), upStream);
    }
    
    Action readResponse() {
      return oatpp::data::stream::
      readSomeDataAsyncInline(m_connection.get(), m_bufferPointer, m_bufferBytesLeftToRead, yieldTo(&ExecutorCoroutine::parseResponse));
    }
    
    Action parseResponse() {
      
      os::io::Library::v_size readCount = m_ioBuffer->getSize() - m_bufferBytesLeftToRead;
      
      if(readCount > 0) {
        
        oatpp::parser::ParsingCaret caret((p_char8) m_ioBuffer->getData(), m_ioBuffer->getSize());
        auto line = protocol::http::Protocol::parseResponseStartingLine(caret);
        if(!line){
          return error("Invalid starting line");
        }
        
        oatpp::web::protocol::http::Status err;
        auto headers = protocol::http::Protocol::parseHeaders(caret, err);
        
        if(err.code != 0){
          return error("can't parse headers");
        }
        
        auto bodyStream = oatpp::data::stream::InputStreamBufferedProxy::createShared(m_connection,
                                                                                      m_ioBuffer,
                                                                                      caret.getPosition(),
                                                                                      (v_int32) readCount);
        
        return _return(Response::createShared(line->statusCode, line->description, headers, bodyStream, m_bodyDecoder));
        
      }
      
      return error("Read zero bytes from Response");
      
    }
    
  };
  
  return parentCoroutine->startCoroutineForResult<ExecutorCoroutine>(callback, m_connectionProvider, method, path, headers, body, m_bodyDecoder, connectionHandle);
  
}
  
}}}
