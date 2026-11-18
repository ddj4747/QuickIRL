#pragma once

#include <ConnectionHandler.h>
#include <PortForwardUtils.h>
#include <StreamHandler.h>
#include <vector>

class UpnpConnectionHandler final : public ConnectionHandler {
public:
	using ConnectionHandler::ConnectionHandler;
	~UpnpConnectionHandler() override;
	void start(const std::string &streamId, StreamProtocol protocol) override;
	void stop() override;
	uint16_t localSrtPort() const override;

private:
	struct ConnectionPort {
		uint16_t port;
		PortForwarder::Protocol protocol;
	};

	void startStream();
	static std::vector<ConnectionPort> forwardingPorts(StreamProtocol protocol);
	std::vector<PortForwarder *> m_forwarders;
	size_t m_pending = 0;
	std::string m_streamId;
	StreamProtocol m_protocol{};
	StreamHandler *m_streamHandler{nullptr};
};
