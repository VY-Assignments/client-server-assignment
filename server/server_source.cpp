#include <iostream>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <string>
#include "..//client-server-assignment/json.hpp"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

enum class Command : int
{
	kGet = 0,
	kList,
	kPut,
	kDelete,
	kInfo,
	kDefault

};

enum class StatusCode : int
{
	kStatusOK = 200,
	kStatusNotFound = 404,
	kStatusCreated = 201,
	kStatusFailure = 500
};

struct JsonFields
{
	const string kMessage = "message";
	const string kStatusCode = "statusCode";
	const string kTransferPort = "transferPort";
	const string kCommand = "command";
	const string kArgument = "argument";
	const string kVersion = "version";
	const string kUniqueID = "uniqueID";
	const string kFileSize = "fileSize";
	const string kFileNames = "fileNames";
	const string kFileInfo = "fileInfo";
	const string kNickname = "nickname";
};



class ServerTcp
{
	optional<shared_ptr<ServerTcp>> initServer(const unsigned int controlPort, const unsigned int transferPort)
	{
		if (thisServerPtr != nullptr)
		{
			return thisServerPtr;
		}
		
		int result = 0;
		if (!isLibLoaded)
		{
			result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		}
		if (result != 0)
		{
			return nullopt;
		}

		isLibLoaded = true;
		thisServerPtr = shared_ptr<ServerTcp>(ServerTcp());

		if (!thisServerPtr->initSockets(controlPort, transferPort))
		{
			thisServerPtr = nullptr;
			return nullopt;
		}

		return thisServerPtr;
	}
	
	bool initSockets(const unsigned short controlPort, const unsigned short transferPort)  // got to make private 
	{
		serverControlSocket = socket(AF_INET, SOCK_STREAM, 0);
		serverTransferSocket = socket(AF_INET, SOCK_STREAM, 0);

		if (serverControlSocket == INVALID_SOCKET || serverTransferSocket == INVALID_SOCKET)
		{
			cerr << format("Error at {}, could not init sockets\n", __func__);
			dropAllConnections();
			return false;
		}
		serverControlPort = controlPort;
		serverTransferPort = transferPort;
	}

	bool bindSockets(const unsigned short controlPort, const unsigned short transferPort)
	{
		if (serverControlSocket == INVALID_SOCKET || serverTransferSocket == INVALID_SOCKET)
		{
			cerr << format("Error ar {}, sockets were invalid, could not bind\n", __func__);
			dropAllConnections();
			return false;
		}

		serverAddr.sin_port = htons(controlPort);

		int resultControl = ::bind(serverControlSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));

		serverAddr.sin_port = htons(transferPort);
		
		int resultTransfer = ::bind(serverTransferSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
		if (resultControl == SOCKET_ERROR || resultTransfer == SOCKET_ERROR)
		{
			cerr << format("Error at {}, bind failed with error: {}\n", __func__, WSAGetLastError());
			dropAllConnections();
			return false;
		}
		
		return true;
	}

	bool startListening()
	{
		if (mainHandlerThreadPtr == nullptr)
		{
			cout << "Already listening\n";
			return false;
		}
		int resultControl = listen(serverControlSocket, SOMAXCONN);
		int resultTransfer = listen(serverTransferSocket, SOMAXCONN);

		if (resultControl == SOCKET_ERROR || resultTransfer == SOCKET_ERROR)
		{
			cerr << format("Error at {}, listen failed with error: {}\n", __func__, WSAGetLastError());
			dropAllConnections();
			return false;
		}

		mainHandlerThreadPtr = make_shared<thread>(([this]() 
			{
				handleNewConnection();
			}));

	}

	~ServerTcp()
	{
		dropAllConnections();
		if (isLibLoaded)
		{
			WSACleanup();
		}
	}

private:
	ServerTcp() 
	{
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_addr.s_addr = INADDR_ANY;
	}

	void dropAllConnections()
	{
		closesocket(serverControlSocket);
		closesocket(serverTransferSocket);

		serverControlSocket = INVALID_SOCKET;
		serverTransferSocket = INVALID_SOCKET;

		isConnected = false;
	}

	bool isConnectionValid() const noexcept
	{
		return serverControlSocket != INVALID_SOCKET && serverTransferSocket != INVALID_SOCKET && isConnected;
	}

	void handleNewConnection()
	{
		SOCKET clientControlSocket;
		while (!ifExit.load())
		{
			clientControlSocket = accept(serverControlSocket, nullptr, nullptr);
			if (clientControlSocket == INVALID_SOCKET)
			{
				cerr << format("Error at {}, could not accept client\n", __func__);
				continue;
			}

			handleClientHandshake(clientControlSocket, generateUniqueId());
		}
	}

	bool tryHandShake()
	{
		// GOT to make the client init them by nick, not by id
		

	}

	int generateUniqueId() const
	{
		int id = 1;
		while (alreadyUsedId.contains(id) && alreadyUsedId.size() < kUniqueIdUpBound)
		{
			random_device rd;
			mt19937 gen(rd());
			uniform_int_distribution<int> dist(1, kUniqueIdUpBound);
			id = dist(gen);
		}

		alreadyUsedId.insert(id);
		
		return id;
	}

	void handleClientHandshake(const SOCKET clientControlSocket, const int uniqueID)
	{
		nlohmann::json responseJson, requestJson = getServerJsonTemplate();

		requestJson[jsFields.kMessage] = kServerHandShakePhrase;
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		int bytesReceived = 0;
		send(clientControlSocket, requestJson.dump().c_str(), requestJson.dump().size(), 0);

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytesReceived == 0)
		{
			cout << format("Error at {}, client disconnected\n", __func__);
			return;
		}
		responseJson = nlohmann::json::parse(buffer);
		if (!ifClientJsonValid(responseJson) || responseJson.value(jsFields.kMessage, "default phrase") != kClientHandShakePhrase)
		{
			cerr << format("Error at {}, client json is not valid\n", __func__);
			dropClient(clientControlSocket, INVALID_SOCKET);
			return;
		}
		requestJson[jsFields.kMessage] = "";
		requestJson[jsFields.kUniqueID] = uniqueID;
		requestJson[jsFields.kTransferPort] = 

		send(clientControlSocket, requestJson.dump().c_str(), requestJson.dump().size(), 0);

	}

	static bool ifClientJsonValid(nlohmann::json jsObject)
	{
		return 
			jsObject.contains(jsFields.kArgument) &&
			jsObject.contains(jsFields.kCommand) &&
			jsObject.contains(jsFields.kFileSize) &&
			jsObject.contains(jsFields.kUniqueID) &&
			jsObject.contains(jsFields.kVersion);
	}

	void dropClient(SOCKET clientControlSocket, SOCKET clientTransferSocket)
	{
		closesocket(clientControlSocket);
		closesocket(clientTransferSocket);
	}

	static nlohmann::json getServerJsonTemplate()  
	{
		return {{jsFields.kStatusCode, StatusCode::kStatusOK}}; 
	}
	static inline const int kUniqueIdUpBound = 65535;

	static inline const JsonFields jsFields;
	static inline const string kServerHandShakePhrase = "Hello client";
	static inline const string kClientHandShakePhrase = "Hello server";
	static inline const string kServerConfirmationPhrase = "OK";
	static inline const string kCommandDelimiter = "||";
	//static inline const int	   kClientVersion = 1;
	static inline const int	   kMaxRetryCounter = 4;
	static inline const int	   kMaxFileNameSize = 15;
	static inline const int    kMinFileNameSize = 1;
	
	static inline bool	  isLibLoaded = false;
	static inline bool	  isConnected = false;
	static inline WSADATA wsaData;
	static sockaddr_in serverAddr;
	static inline atomic<bool> ifExit = false ;
	static inline int serverControlPort;
	static inline int serverTransferPort;

	static inline shared_ptr<ServerTcp> thisServerPtr = nullptr;
	static inline shared_ptr<thread> mainHandlerThreadPtr = nullptr;

	static inline SOCKET serverControlSocket = INVALID_SOCKET;
	static inline SOCKET serverTransferSocket = INVALID_SOCKET;

	unordered_map<int, SOCKET> idToClintControlSocket;
	unordered_map<int, SOCKET> idToClientTransferSocket;
	unordered_map<int, string> idToNickName;

	unordered_set<int> alreadyUsedId;

	vector<thread> clientsThreadVec;

	
};
vector<string> listDir(const string& directoryPath = ".")
{
	vector<string> result;

	if (!filesystem::exists(directoryPath) || !filesystem::is_directory(directoryPath)) 
	{
		cerr << "Error: Directory does not exist or is not a directory!\n";
		return result;  
	}

	for (const auto& entry : filesystem::directory_iterator(directoryPath)) 
	{
		 result.push_back(entry.path().filename().string());  // Print only the file name
	}
	return result;
}

/*
chrono::system_clock::time_point getLastModifiedTime(const string& filePath) 
{
	if (!filesystem::exists(filePath)) 
	{
		cerr << "File does not exist!\n";
		return chrono::system_clock::time_point{};  // Return an empty (epoch) time_point
	}

	auto ftime = filesystem::last_write_time(filePath);
	auto systemTime = chrono::time_point_cast<chrono::system_clock::duration>(ftime - filesystem::file_time_type::clock::now() + chrono::system_clock::now());

	return systemTime;
}
*/




int main()
{
	return 0;
}
