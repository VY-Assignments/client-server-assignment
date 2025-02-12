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


namespace common
{

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
	
	inline size_t getFileSize(std::ifstream& file)
	{
		std::streampos current = file.tellg();
		file.seekg(0, std::ios::end);
		size_t size = file.tellg();
		file.seekg(current, std::ios::beg);

		return size;
	}


}


using namespace std;
using namespace common;
namespace server_utils
{
	struct DefaultJsonFields   
	{
		const string			 kMessage	   = "";
		const common::StatusCode kStatusCode   = StatusCode::kStatusOK;
		const int				 kTransferPort = -1;
		const int				 kUniqueID	   = -1;
		const int				 kFileSize	   = -1;
		const string			 kFileName	   = "";
		const string			 kFileInfo	   = "";
		const string			 kNickname	   = "";
		const Command			 kCommand	   = Command::kDefault;
	};

	
}

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
		thisServerPtr = shared_ptr<ServerTcp>(new ServerTcp());

		if (!thisServerPtr->initSockets())
		{
			thisServerPtr = nullptr;
			return nullopt;
		}

		return thisServerPtr;
	}
	
	bool initSockets(/*const unsigned short controlPort, const unsigned short transferPort*/)  // got to make private 
	{
		serverControlSocket = socket(AF_INET, SOCK_STREAM, 0);
		serverTransferSocket = socket(AF_INET, SOCK_STREAM, 0);

		if (serverControlSocket == INVALID_SOCKET || serverTransferSocket == INVALID_SOCKET)
		{
			cerr << format("Error at {}, could not init sockets\n", __func__);
			dropAllConnections();
			return false;
		}
	}

	bool bindSockets(const unsigned short controlPort, const unsigned short transferPort)
	{
		if (serverControlSocket == INVALID_SOCKET || serverTransferSocket == INVALID_SOCKET)
		{
			cerr << format("Error ar {}, sockets were invalid, could not bind\n", __func__);
			dropAllConnections();
			return false;
		}

		serverControlPort = controlPort;
		serverTransferPort = transferPort;

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
		if (mainListeningThreadPtr == nullptr)
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

		mainListeningThreadPtr = make_shared<thread>(([this]() 
			{
				acceptNewConnection();
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

	void acceptNewConnection()
	{
		SOCKET clientControlSocket, clientTransferSocket;
		while (!ifExit.load())
		{
			clientControlSocket = accept(serverControlSocket, nullptr, nullptr);
			if (clientControlSocket == INVALID_SOCKET)
			{
				cerr << format("Error at {}, could not accept client\n", __func__);
				continue;
			}

			int curClientID = generateUniqueId();

			if (!handleClientHandshake(clientControlSocket, curClientID))
			{
				cout << format("Error at {}, handshake failed with error {}\n", __func__, WSAGetLastError());
				continue;
			}

			clientTransferSocket = handleClientConnectToTransferPort(curClientID);
			if (clientTransferSocket == INVALID_SOCKET)
			{
				dropClient(clientControlSocket, INVALID_SOCKET);
				continue;
			}
			
			idToClintControlSocket[curClientID] = clientControlSocket;
			idToClientTransferSocket[curClientID] = clientTransferSocket;

			handleClient(curClientID);
		}


	}

	void handleClient(const int curClientID)
	{
		int bytesSent = 0, bytesReceived = 0;
		
		SOCKET clientControlSocket = idToClintControlSocket[curClientID];
		
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer), 0);

		while(bytesReceived != 0) 
		{
			nlohmann::json clientJson = nlohmann::json::parse(buffer);
			if (clientJson.value(jsFields.kUniqueID, jsFieldsDefault.kUniqueID) == jsFieldsDefault.kUniqueID || 
				clientJson.value(jsFields.kCommand, jsFieldsDefault.kCommand) == jsFieldsDefault.kCommand)
			{
				cerr << format("Error at {}, incorrect client json\n", __func__);
				dropClient(clientControlSocket, idToClientTransferSocket[curClientID]);
			}
			Command curCommand = clientJson[jsFields.kCommand];
			bool result = false;
			switch (curCommand)
			{
			case Command::kGet: 
				result = handleGetFromServer(clientJson, curClientID);
				break;
			case Command::kInfo:
				result = handleGetFileInfo(clientJson, curClientID);
				break;
			case Command::kPut:
				result = handlePutFile(clientJson, curClientID);
				break;
			case Command::kList:
				result = handleListDir(clientJson, curClientID);
				break;
			default:
				cerr << format("Error at {}, command not implemented", __func__);
				break;
			}
			(result) ? cout << format("Success, command {} executed by user {}", curCommand, curClientID) : cout << format("Failed: command {} failed by user {}", curCommand, curClientID);
		}


	
	
	}

	bool handleGetFromServer(const nlohmann::json& clientJson, const int& curClientID)
	{
		string fileName = clientJson.value(jsFields.kArgument, "");

		if (fileName == "")
		{
			cout << format("Error at {} in request by user {}, file name Unknown or Empty\n", __func__, curClientID);
			auto serverJson = getServerJsonTemplate(format("Error in request by user {}, file name Unknown or Empty", curClientID), StatusCode::kStatusFailure, curClientID);
			send(idToClintControlSocket[curClientID], serverJson.dump().c_str(), serverJson.dump().size(), 0);	
			
			return false;
		}

		ifstream file(fileName, ios::binary);

		if (!file)
		{
			cout << format("Error at {}, file {} not present in dir\n", __func__, fileName);
			auto serverJson = getServerJsonTemplate(format("Error, could not find the file in DIR"), StatusCode::kStatusNotFound, curClientID);
			send(idToClintControlSocket[curClientID], serverJson.dump().c_str(), serverJson.dump().size(), 0);
			return false;
		}
		
		auto serverJson = getServerJsonTemplate("", StatusCode::kStatusOK, curClientID, "", static_cast<long long>(common::getFileSize(file)) );
		send(idToClintControlSocket[curClientID], serverJson.dump().c_str(), serverJson.dump().size(), 0);

		if (transferFileToClient(curClientID))
	
		return true;
	}
	bool handleGetFileInfo(const nlohmann::json& clientJson, const int& curClientID)
	{


		return true;
	}

	bool transferFileToClient(const int curClientID)
	{
		SOCKET clientTransferSock = idToClientTransferSocket[curClientID];



		
		
	}

	bool tryHandShake()
	{
		// GOT to make the client init them by nick, not by id (will implemnt it in 4 assignment)
		nlohmann::json requestJson = getServerJsonTemplate();
		


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

	bool handleClientHandshake(const SOCKET clientControlSocket, const int uniqueID)
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
			return false;
		}
		responseJson = nlohmann::json::parse(buffer);
		if (!ifClientJsonValid(responseJson) || responseJson.value(jsFields.kMessage, "default phrase") != kClientHandShakePhrase)
		{
			cerr << format("Error at {}, client json is not valid\n", __func__);
			dropClient(clientControlSocket, INVALID_SOCKET);
			return false;
		}
		requestJson[jsFields.kMessage] = "";
		requestJson[jsFields.kUniqueID] = uniqueID;
		requestJson[jsFields.kTransferPort] = serverTransferPort;
		
		return send(clientControlSocket, requestJson.dump().c_str(), requestJson.dump().size(), 0) != 0;
	}

	SOCKET handleClientConnectToTransferPort(const int uniqueID)  // if I accept multiple clients on transfer port in multiple threads, I need a mutex and a notifier to notify the waiting thread that the client has connected or that the timeou thas passed
	{
		SOCKET clientTransferSocket = INVALID_SOCKET;
			
		auto startStamp = chrono::high_resolution_clock::now();

		bool ifSuccess = false; 
		
		DWORD socketTimeout = static_cast<DWORD>(chrono::duration_cast<chrono::milliseconds>(kTimeout).count());

		while (!ifSuccess && chrono::duration_cast<chrono::seconds>(chrono::high_resolution_clock::now() - startStamp) < kTimeout)
		{
			clientTransferSocket = INVALID_SOCKET;

			setsockopt(serverTransferSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&socketTimeout), sizeof(socketTimeout));
			clientTransferSocket = accept(serverTransferSocket, nullptr, nullptr);

			socketTimeout = 0;
			setsockopt(serverTransferSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&socketTimeout), sizeof(socketTimeout));
			
			if (clientTransferSocket == INVALID_SOCKET)
				continue;

			char buffer[1024];
			memset(buffer, 0, sizeof(buffer));

			int bytesReceived = recv(clientTransferSocket, buffer, sizeof(buffer) - 1, 0);

			if (bytesReceived == 0)
			{
				closesocket(clientTransferSocket);
				continue;
			}
			
			nlohmann::json responseJson = nlohmann::json::parse(buffer);
			ifSuccess = (
				responseJson.value(jsFields.kUniqueID, -1) == uniqueID && 
				responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure) == StatusCode::kStatusOK);
			
		}

		if (!ifSuccess)
		{
			cout << format("Could not connect to transfer port, aborting..\n");
		}

		return clientTransferSocket;
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

	static nlohmann::json getServerJsonTemplate(
		const string message = jsFieldsDefault.kMessage,
		const common::StatusCode statusCode = jsFieldsDefault.kStatusCode,  
		const int id = jsFieldsDefault.kUniqueID, 
		string nickName = jsFieldsDefault.kNickname,
		const int fileSize = jsFieldsDefault.kFileSize,
		string fileName = jsFieldsDefault.kFileName, 
		string fileInfo = jsFieldsDefault.kFileInfo)  
	{
		nlohmann::json result;

		result[jsFields.kStatusCode] = statusCode;  // mandatory add the status code
		
		if (message != jsFieldsDefault.kMessage)
			result[jsFields.kMessage] = message;

		if (id != jsFieldsDefault.kUniqueID)
			result[jsFields.kUniqueID] = id;

		if (fileSize != jsFieldsDefault.kFileSize)
			result[jsFields.kFileSize] = fileSize;

		if (fileName != jsFieldsDefault.kFileName)
			result[jsFields.kArgument] = fileName;

		if (fileInfo != jsFieldsDefault.kFileInfo)
			result[jsFields.kFileInfo] = fileInfo;

		if (nickName != jsFieldsDefault.kNickname)
			result[jsFields.kNickname] = nickName;

		return result;
	}

	static inline const int kUniqueIdUpBound = 65535;
	static inline chrono::seconds kTimeout{ 5 };

	static inline const JsonFields jsFields;
	static inline const server_utils::DefaultJsonFields jsFieldsDefault;

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
	static inline shared_ptr<thread> mainListeningThreadPtr = nullptr;

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
	listDir();
	//getLastModifiedTime("./");
	return 0;
	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		std::cerr << "WSAStartup failed" << std::endl;
		return 1;
	}
	// Server configuration
	int port = 12345;
	SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (serverSocket == INVALID_SOCKET)
	{
		std::cerr << "Error creating socket: " << WSAGetLastError() << std::endl;
		WSACleanup();
		return 1;
	}
	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(port);
	// Bind the socket
	if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
	{
		std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
		closesocket(serverSocket);
		WSACleanup();
		return 1;
	}
	// Listen for incoming connections
	if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(serverSocket);
		WSACleanup();
		return 1;
	}
	std::cout << "Server listening on port " << port << std::endl;
	// Accept a client connection
	SOCKET clientControlSocket = accept(serverSocket, nullptr, nullptr);
	if (clientControlSocket == INVALID_SOCKET)
	{
		std::cerr << "Accept failed with error: " << WSAGetLastError() << std::endl;
		closesocket(serverSocket);
		WSACleanup();
		return 1;
	}
	// Receive data from the client
	char buffer[1024];
	DWORD timeout = 800; // 5 seconds
	setsockopt(clientControlSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
	ofstream file("photo1.jpg", ios::binary);
	while(true)
	{
		memset(buffer, 0, sizeof(buffer));
		int bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytesReceived > 0)
		{
			file.write(buffer, bytesReceived);

			std::cout << "Received data: " << buffer << std::endl;
			// Send a response back to the client
			
		}
		else
		{
			string response = "Hello, client! This is the server.";
			send(clientControlSocket, response.c_str(), (int)response.size(), 0);
			break;
		}
	}

	
	// Clean up
	closesocket(clientControlSocket);
	closesocket(serverSocket);
	WSACleanup();
	return 0;
}
