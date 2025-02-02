#include <iostream>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <vector>
#include <tuple>
#include <optional>
#include <conio.h>
//#include <memory>
#include <format>

#pragma comment(lib, "ws2_32.lib")
using namespace std;
// using std::vector, std::cout, std::cerr, std::optional, std::nullopt, std::shared_ptr;

static bool isEqual(const int result, const int expected)
{
	return result == expected;
}
static void cleanUpResources(const vector<SOCKET> socketsToClose)
{
	for (auto& socket : socketsToClose)
	{
		if (socket != INVALID_SOCKET)
		{
			closesocket(socket);
		}
	}
	WSACleanup();
}
/*
class ClientTcp
{
public:

	static optional<shared_ptr<ClientTcp>> initClient()
	{
		int result;
		if (!isLibLoaded)
		{
			result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		}
		
		if (result != 0)
		{
			return std::nullopt;
		}

		isLibLoaded = true;
		
		return make_shared<ClientTcp>();
	}
	
	bool initSocket() noexcept
	{
		clientSocket = socket(AF_INET, SOCK_STREAM, 0);

		if (clientSocket == INVALID_SOCKET)
		{
			return false;
		}
		return true;
	}
	
	bool tryConnectToServer(const int serverPort, const PCWSTR serverIP) noexcept
	{
		if (isConnected)
		{
			cerr << format("Error at {}, could not connect, already connected\n", __func__);
			return false;
		}
		if (clientSocket == INVALID_SOCKET && !initSocket())
		{
			cerr << format("Error ar {}, could not init socket object, error: {}\n", __func__, WSAGetLastError());
			return false;	
		}


		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(serverPort);
		InetPton(AF_INET, serverIP, &serverAddr.sin_addr);

		if (connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
		{
			cerr << format("Error at {}, connection failed with error: {}\n", __func__, WSAGetLastError());
			isConnected = false;
			return false;
		}

		isConnected = true;
		
		if (!tryHandShake())
		{
			cerr << format("Error at {}, server does not follow handshake\n", __func__);
			isConnected = false;
		}
		wcout << format(L"SUCCESS, connected to {}:{}\n", serverIP, serverPort);
		isConnected = true;
		
		return true;
	}

	bool tryHandShake()
	{
		if (!isConnected)
		{
			cerr << format("Error ar {}, hand shake failed, try to connect first\n", __func__);
			return false;
		}
		int bytesReceived = 0;
		char buffer[1024];
		string response;

		for (int index = 0; index < 2; index++)
		{
			bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0) > 0;
			
			if (bytesReceived == 0)
			{
				return false;
			}
			buffer[bytesReceived] = '\0';

			response = buffer;

			if (index == 0) 
			{
				if (response != SERVER_HANDSHAKE_PHRASE)
				{
					return false;
				}
				send(clientSocket, CLIENT_HANDSHAKE_PHRASE.c_str(), (int)CLIENT_HANDSHAKE_PHRASE.size(), 0);
			}
			else if (index == 1)
			{
				if (response != "OK")
				{
					return false;
				}
				
			}
		}
		
		return true;
	}
	

	~ClientTcp()
	{
		if (isLibLoaded)
		{
			WSACleanup();
		}
		if (clientSocket != INVALID_SOCKET)
		{
			closesocket(clientSocket);
		}
	}
private:
	ClientTcp() {}

	sockaddr_in serverAddr;

	static inline bool isLibLoaded = false;
	static inline WSADATA wsaData;

	static inline const string SERVER_HANDSHAKE_PHRASE = "Hello client";
	static inline const string CLIENT_HANDSHAKE_PHRASE = "Hello server";
	static inline const string SERVER_CONFIRMATION_PHRASE = "OK";
	
	bool isConnected = false;
	int serverPort;
	PCWSTR serverIP;
	SOCKET clientSocket = INVALID_SOCKET;
	

};
*/


/*
const int CLIENT_PORT = 12340;
const PCWSTR SERVER_IP = L"127.0.0.1";
SOCKET clientSocket = INVALID_SOCKET;
*/

int main()
{
	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		std::cerr << "WSAStartup failed" << std::endl;
		return 1;
	}


	// Client configuration

	int port = 12345;
	PCWSTR serverIp = L"127.0.0.1";
	SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);

	if (clientSocket == INVALID_SOCKET)
	{
		std::cerr << "Error creating socket: " << WSAGetLastError() << std::endl;
		cleanUpResources({});
		return 1;
	}
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(port);
	InetPton(AF_INET, serverIp, &serverAddr.sin_addr);
	// Connect to the server
	// can be used to set timeout
	DWORD timeout = 800; // 5 seconds
	setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

	if (connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
	{
		std::cerr << "Connect failed with error: " << WSAGetLastError() << std::endl;
		closesocket(clientSocket);
		WSACleanup();
		return 1;
	}
	// Send data to the server
	const char* message = "Hello, server! How are you?";
	send(clientSocket, message, (int)strlen(message), 0);
	// Receive the response from the server
	char buffer[1024];
	memset(buffer, 0, 1024);
	int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
	if (bytesReceived > 0)
	{
		std::cout << "Received from server: " << string(buffer) << std::endl;
	}
	// Clean up
	closesocket(clientSocket);
	WSACleanup();
	return 0;
}


