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
#include <regex>
#include <vector>  
#include <string>
#include <mutex>
#include "..//client-server-assignment/common.hpp"


#pragma comment(lib, "ws2_32.lib")
using namespace std;



namespace server_utils
{
	struct DefaultJsonFields   
	{
		const string			 kMessage	   = "";
		const string			 kArgument	   = "";
		const common::StatusCode kStatusCode   = common::StatusCode::kStatusOK;
		const int				 kTransferPort = -1;
		const int				 kUniqueID	   = -1;
		const int				 kFileSize	   = -1;
		const string			 kFileName	   = "";
		const string			 kFileInfo	   = "";
		const string			 kNickname	   = "";
		const common::Command	 kCommand	   = common::Command::kDefault;
	};
}


vector<string> listDir(string directoryPath = ".")
{
	vector<string> result;

	if (!filesystem::exists(directoryPath) || !filesystem::is_directory(directoryPath))
	{
		cerr << "Error: Directory does not exist or is not a directory!\n";
		return result;
	}

	for (auto& entry : std::filesystem::directory_iterator(directoryPath))
	{
		result.push_back(entry.path().filename().string());  // print only the file name
	}
	return result;
}

class ServerTcp
{
public:
	static optional<shared_ptr<ServerTcp>> initServer(string configName = "config.txt")
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
		if (!fileExists(configName) || result != 0)
		{
			return nullopt;
		}

		isLibLoaded = true;
		thisServerPtr = shared_ptr<ServerTcp>(new ServerTcp(configName));
		ifstream file(configName);
		
		if (!thisServerPtr->initSockets() || !thisServerPtr->loadConfig(file))
		{
			thisServerPtr = nullptr;
			return nullopt;
		}
		thisServerPtr->parseConfigCreateDir();

		return thisServerPtr;
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
			if (controlListeningThreadPtr != nullptr)
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

			controlListeningThreadPtr = make_shared<thread>(([this]() 
				{
					acceptNewControlConnection();
				}));
			transferListeningThreadPtr = make_shared<thread>([this]()
				{
					acceptNewTransferConnection();
				});

		}

		void acceptNewControlConnection()
		{
			SOCKET clientControlSocket = INVALID_SOCKET, clientTransferSocket = INVALID_SOCKET;

			while (!ifExit.load())
			{
				clientControlSocket = tryAcceptClientSocket(serverControlSocket);

				if (clientControlSocket == INVALID_SOCKET)
				{
					// cerr << format("Error at {}, could not accept client\n", __func__);
					continue;
				}

				int curClientID = generateUniqueId();

				clientsThreadVec.emplace_back([this, clientControlSocket, curClientID]()
					{
						handleNewControlConnection(clientControlSocket, curClientID);
					});

			}
		}

		void handleNewControlConnection(SOCKET clientControlSocket, const int curClientID) 
		{
			if (!handleClientHandshakeAndRecordNickName(clientControlSocket, curClientID))   // record name here
			{
				cout << format("Error at {}, handshake failed with error {}\n", __func__, WSAGetLastError());
				dropClient(curClientID);
				return;
			}
			
			{
				scoped_lock lock(sessionIDToNickNameMapMtx, curLoginedUsersSetMtx, sessionIDToControlSocketMapMtx, sessionWaitingForTransferMtx);
				curLoginedUsersNamesSet.insert(sessionIDToNickNameMap[curClientID]);
				sessionIDToClientControlSocket[curClientID] = clientControlSocket;
				sessionsWaitingForTransferConnection.insert(curClientID);
			}

			/*
			clientTransferSocket = acceptNewTransferConnection(curClientID);
			if (clientTransferSocket == INVALID_SOCKET)
			{
				dropClient(curClientID);
				continue;
			}
			*/

			handleClient(curClientID);
		}

		~ServerTcp()
		{
			ifExit = true;

			dropAllConnections();


			if (controlListeningThreadPtr != nullptr 
				&& transferListeningThreadPtr != nullptr 
				&& controlListeningThreadPtr->joinable() 
				&& transferListeningThreadPtr->joinable())
			{
				controlListeningThreadPtr->join();
				transferListeningThreadPtr->join();
			}
			for (auto& th : clientsThreadVec)
			{
				if (th.joinable())
				{
					th.join();
				}
			}
			if (isLibLoaded)
			{
				WSACleanup();
			}

		}



private:

	friend class shared_ptr<ServerTcp>;
	
	explicit ServerTcp(const string& configName) : kConfigName(configName)
	{
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_addr.s_addr = INADDR_ANY;

	}

	
	void dropAllConnections()   // not a thread - safe operation
	{
		shutdown(serverControlSocket, SD_BOTH);
		shutdown(serverTransferSocket, SD_BOTH);

		closesocket(serverControlSocket);
		closesocket(serverTransferSocket);

		serverControlSocket = INVALID_SOCKET;
		serverTransferSocket = INVALID_SOCKET;

		isConnected = false;
	}

	bool isConnectionValid() const noexcept
	{
		return serverControlSocket != INVALID_SOCKET && serverTransferSocket != INVALID_SOCKET;
	}


	void handleClient(const int curClientID)   
	{
		int bytesSent = 0, bytesReceived = 0, retryCounter = 0;
		SOCKET clientControlSocket = INVALID_SOCKET, clientTransferSocket = INVALID_SOCKET;

		{
			lock_guard lock(sessionIDToControlSocketMapMtx);

			if (sessionIDToClientControlSocket.contains(curClientID))
				clientControlSocket = sessionIDToClientControlSocket[curClientID];
		}

		if (clientControlSocket == INVALID_SOCKET)
		{
			cerr << format("Error at {}, client disconnecrted\n", __func__);
			return;
		}
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		
		while(retryCounter <= kMaxRetryCounter && !ifTransferSocketLoaded(curClientID))  // try check for loaded transfer socket
		{
			retryCounter++;
			cerr << format("Error at {}, transfer socket for client {} was not loaded\n", __func__, curClientID);
			if (retryCounter == kMaxRetryCounter)
			{
				cerr << format("Error at {}, could not load transfer socket, droping client {}\n", __func__, curClientID);

				auto serverJsonStr = getServerJsonTemplate(format("Could not load transfer socket", curClientID), common::StatusCode::kStatusFailure, curClientID).dump() + kCommandDelimiter;
				send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);

				dropClient(curClientID);
				return;
			}
			cerr << format("Retrying to load transfer socket for client {}\n", curClientID);
			this_thread::sleep_for(100ms);
		}
	

		while((bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer), 0)) 
			&& bytesReceived != 0 
			&& !ifExit.load())
		{
			string receivedStr = buffer;
			nlohmann::json clientJson = nlohmann::json::parse(receivedStr.substr(0, receivedStr.find(kCommandDelimiter)));

			if (clientJson.value(jsFields.kUniqueID, jsFieldsDefault.kUniqueID) == jsFieldsDefault.kUniqueID || 
				clientJson.value(jsFields.kCommand, jsFieldsDefault.kCommand) == jsFieldsDefault.kCommand)
			{
				cerr << format("Error at {}, incorrect client json\n", __func__);
				dropClient(curClientID);
				return;
			}

			common::Command curCommand = clientJson[jsFields.kCommand];
			bool result = false;

			switch (curCommand)
			{
			case common::Command::kGet: 
				result = handleGetFromServer(clientJson, curClientID);
				break;
			case common::Command::kInfo:
				result = handleGetFileInfo(clientJson, curClientID);
				break;
			case common::Command::kPut:
				result = handlePutFile(clientJson, curClientID);
				break;
			case common::Command::kList:
				result = handleListDir(clientJson, curClientID);
				break;
			case common::Command::kDelete:
				result = handleDeleteFile(clientJson, curClientID);
				break;
			default:
				cerr << format("Error at {}, command not implemented", __func__);
				break;
			}
			
			(result) ? cout << format("Success, command {} executed by user {}\n", static_cast<int>(curCommand), curClientID) : cout << format("Failed: command {} failed by user {}\n", static_cast<int>(curCommand), curClientID);
			memset(buffer, 0, sizeof(buffer));

			//bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer), 0);
		}
		dropClient(curClientID);
		cout << format("Client {} disconnected, session closed\n", curClientID);
	}

	bool handleDeleteFile(nlohmann::json clientJson, const int curClientID)
	{
		const string fileName = clientJson.value(jsFields.kArgument, "");
		const string filePath = sessionIDToNickNameMap[curClientID] + "/" + fileName;

		if (!ifFileNameValid(fileName) || !fileExists(filePath))
		{
			cout << format("Error at {} in request by user {}, file name Invalid or file not found\n", __func__, curClientID);

			auto serverJsonStr = getServerJsonTemplate(format("Error in request, file name Invalid or file not found"), common::StatusCode::kStatusNotFound, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);

			return false;
		}
		try 
		{
			filesystem::remove(filePath);
		}
		catch (const filesystem::filesystem_error& e) 
		{
			cerr << format("Error at {} deleting file: {}, by client {}\n", __func__, e.what(), curClientID);
			auto serverJsonStr = getServerJsonTemplate(format("Error deleting file: {}", e.what()), common::StatusCode::kStatusFailure, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);
			return false;
		}
		auto serverJsonStr = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID).dump() + kCommandDelimiter;
		send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);
		
		return true;
	}


	bool handleGetFromServer(const nlohmann::json& clientJson, const int& curClientID)
	{
		const string fileName = clientJson.value(jsFields.kArgument, "");
		
		const string filePath = sessionIDToNickNameMap[curClientID] + "/" + fileName;

		bool a = ifFileNameValid(fileName);
		bool b = fileExists(filePath);
		if (!ifFileNameValid(fileName) || !fileExists(filePath))
		{
			cerr << format("Error at {} in request by user {}, file name Invalid or Empty\n", __func__, curClientID);

			auto serverJsonStr = getServerJsonTemplate(format("Error in request by user {}, invalid file name or file not found", curClientID), common::StatusCode::kStatusFailure, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);	
			
			return false;
		}


		ifstream file(filePath, ios::binary);
		
		long long fileSize = static_cast<long long>(common::getFileSize(file));

		auto serverJsonStr = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID, "", fileSize).dump() + kCommandDelimiter;
		send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);

		if (!transferFileToClient(curClientID, file, static_cast<long long>(common::getFileSize(file))))
		{
			cerr << format("Error at {}, issue transferring file {}\n", __func__, fileName);
			dropClient(curClientID);
			return false;
		}

		cout << format("Success: transfered file {} to client with ses ID {}\n", fileName, curClientID);
		return true;
	}

	bool handleGetFileInfo(const nlohmann::json& clientJson, const int& curClientID)
	{
		const string fileName = clientJson.value(jsFields.kArgument, "");
		const string filePath = sessionIDToNickNameMap[curClientID] + "/" + fileName;

		if (!ifFileNameValid(fileName) || !fileExists(filePath))
		{
			cout << format("Error at {} in request by user {}, file name Invalid or file not found\n", __func__, curClientID);

			auto serverJsonStr = getServerJsonTemplate(format("Error in request, file name Invalid or file not found"), common::StatusCode::kStatusFailure, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], (serverJsonStr).c_str(), serverJsonStr.size(), 0);

			return false;
		}
		
		chrono::time_point<chrono::file_clock, chrono::seconds> lastWriteTime;

		try
		{
			auto ftime = filesystem::last_write_time(filePath);
			lastWriteTime = std::chrono::time_point_cast<std::chrono::seconds>(ftime);
		}
		catch (const filesystem::filesystem_error& e)
		{
			string serverJsonStr = getServerJsonTemplate(format("Error getting the write time of file{}", fileName), common::StatusCode::kStatusFailure, curClientID);
			send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);
		}
		
		ifstream file(filePath, ios::binary);

		string info = format("{} | {} bytes", lastWriteTime, common::getFileSize(file));

		string serverJsonStr = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID).dump();
		send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);
		
		
		return transferFileInfo(curClientID, info);;
	}

	bool transferFileToClient(const int curClientID, ifstream& file, const long long fileSize)
	{
		SOCKET clientTransferSock = sessionIDToClientTransferSocket[curClientID];

		const size_t kChunkSize = 1024;
		long long bytesRead = 0, totalRealBytesUploaded = 0;

		char buffer[kChunkSize];
		memset(buffer, 0, sizeof(buffer));

		while (file.read(buffer, kChunkSize) || file.gcount() > 0)
		{
			bytesRead = file.gcount();
			totalRealBytesUploaded += bytesRead;

			send(clientTransferSock, buffer, bytesRead, 0);   // raw 
			memset(buffer, 0, sizeof(buffer));
		}

		return totalRealBytesUploaded == fileSize;
	}

	bool transferFileInfo(const int curClientID, const string& info)
	{
		string serverJsonStr = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID, "", -1, "", info).dump();
		return SOCKET_ERROR != send(sessionIDToClientTransferSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);
	}

	bool handlePutFile(nlohmann::json clientJson, const int curClientID)
	{
		
		if (clientJson.value(jsFields.kArgument, "") == jsFieldsDefault.kArgument || 
			clientJson.value(jsFields.kFileSize, -1)  == jsFieldsDefault.kFileSize || 
			fileExists(sessionIDToNickNameMap[curClientID] + "/" + clientJson.value(jsFields.kArgument, "")))
		{
			cerr << format("Error at {}, invalid file size or file name or file already exists\n", __func__);
			string serverJsonStr = getServerJsonTemplate("Error, invalid file size, name or file already exists", common::StatusCode::kStatusFailure, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);
			return false;
		}
		
		string serverJsonStr = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID).dump() + kCommandDelimiter;
		
		if (SOCKET_ERROR == send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0))
		{
			cerr << format("Erorr at {}, client disconnected, closing session\n", __func__);
			dropClient(curClientID);
			return false;
		}
		
		string curDir = sessionIDToNickNameMap[curClientID];

		if (!filesystem::exists(curDir))
		{
			if (filesystem::create_directories(curDir))
			{
				cout << "Directory created: " << curDir << '\n';
			}
			else
			{
				cerr << "Failed to create directory: " << curDir << '\n';
				return false;
			}
		}

		return acceptFile(curClientID, clientJson[jsFields.kArgument], clientJson[jsFields.kFileSize]);
	}

	bool acceptFile(const int curClientID, const string& fileName, const int fileSize) 
	{
		int bytesReceived = 0, totalRealBytesReceived = 0;

		char buffer[1024]; 
		string dir = sessionIDToNickNameMap[curClientID], filePath = dir + "/" + fileName;

		ofstream newFile(filePath, ios::binary);

		while (totalRealBytesReceived < fileSize && (bytesReceived = recv(sessionIDToClientTransferSocket[curClientID], buffer, sizeof(buffer) - 1, 0)) && bytesReceived != 0)
		{
			newFile.write(buffer, bytesReceived);
			totalRealBytesReceived += bytesReceived;
		}
		newFile.close();

		return true;
	}

	bool handleListDir(nlohmann::json clientJson, const int curClientID)
	{
		string dir = sessionIDToNickNameMap[curClientID];

		if (!filesystem::exists(dir))
		{
			cerr << format("Error at {}, dir {} was not found\n", __func__, dir);
			string serverJsonStr = getServerJsonTemplate(format("Error, dir {} is not found", dir), common::StatusCode::kStatusNotFound, curClientID).dump() + kCommandDelimiter;
			send(sessionIDToClientControlSocket[curClientID], serverJsonStr.c_str(), serverJsonStr.size(), 0);
			return false;
		}
		
		nlohmann::json serverFileInfoJson = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID);
		serverFileInfoJson[jsFields.kFileNames] = listDir(dir);

		nlohmann::json serverJsonResponse = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID, "", serverFileInfoJson.dump().size());
		
		string serverJsonResponseStr = serverJsonResponse.dump() + kCommandDelimiter;

		if (SOCKET_ERROR == send(sessionIDToClientControlSocket[curClientID], serverJsonResponseStr.c_str(), serverJsonResponseStr.size(), 0))
		{
			dropClient(curClientID);
			cerr << format("Error at {}, disconnectiong client\n", __func__);
			return false;
		}
		
		return transferFileList(curClientID, serverFileInfoJson.dump());
	}

	bool transferFileList(const int curClientID, const string& serverJsonStr)
	{
		const size_t kChunkSize = 1024;
		int bytesUploaded = 0;
		size_t totalRealBytesUploaded = 0;

		while (totalRealBytesUploaded < serverJsonStr.size())
		{
			size_t remainingBytes = serverJsonStr.size() - totalRealBytesUploaded;

			size_t chunkSize = min(kChunkSize, remainingBytes);

			string chunk = serverJsonStr.substr(totalRealBytesUploaded, chunkSize);
			
			bytesUploaded = send(sessionIDToClientTransferSocket[curClientID], chunk.c_str(), chunk.size(), 0);

			
			if (bytesUploaded == SOCKET_ERROR)
			{
				std::cerr << std::format("Error at {}, error uploading file list json\n", __func__);
				dropClient(curClientID);
				return false;
			}

			totalRealBytesUploaded += bytesUploaded;
		}

		return true;
		
	}

	bool ifTransferSocketLoaded(const int curClientID)
	{
		scoped_lock lock(sessionIDToTransferSocketMapMtx, sessionWaitingForTransferMtx);
		return !sessionsWaitingForTransferConnection.contains(curClientID) && sessionIDToClientTransferSocket.contains(curClientID);
	}

	int generateUniqueId() 
	{
		int id = 1;
		while (alreadyUsedIDSet.contains(id) && alreadyUsedIDSet.size() < kUniqueIdUpBound - 1)
		{
			random_device rd;
			mt19937 gen(rd());
			uniform_int_distribution<int> dist(1, kUniqueIdUpBound);
			id = dist(gen);
		}
		{
			lock_guard lock(alreadyUsedSessionIDSetMtx);
			alreadyUsedIDSet.insert(id);
		}
		
		return id;
	}

	bool handleClientHandshakeAndRecordNickName(const SOCKET clientControlSocket, const int uniqueID)
	{
		nlohmann::json responseJson, requestJson = getServerJsonTemplate();

		requestJson[jsFields.kMessage] = kServerHandShakePhrase;
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		int bytesReceived = 0;
		send(clientControlSocket, requestJson.dump().c_str(), requestJson.dump().size(), 0);

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);
		string responseStr = buffer;
		responseStr = responseStr.substr(0, responseStr.find(kCommandDelimiter));

		if (bytesReceived == 0)
		{
			cout << format("Error at {}, client disconnected\n", __func__);
			return false;
		}
		responseJson = nlohmann::json::parse(responseStr);

		if (responseJson.value(jsFields.kMessage, "default phrase") != kClientHandShakePhrase 
			/*|| !responseJson.contains(jsFields.kNickname) */ )
		{
			cerr << format("Error at {}, client json is not valid\n", __func__);
			closesocket(clientControlSocket);
			return false;
		}

		string curNickName = responseJson.value(jsFields.kNickname, jsFieldsDefault.kNickname);	
		
		bool ifCurNickNameUsed = false;
		{
			lock_guard lock(curLoginedUsersSetMtx);
			ifCurNickNameUsed = curLoginedUsersNamesSet.contains(curNickName);
		}
		
		if (ifCurNickNameUsed)
		{
			cerr << format("Name {} is already used, drop new client\n", curNickName);
			closesocket(clientControlSocket);
			return false;
		}


		requestJson = getServerJsonTemplate("", common::StatusCode::kStatusOK, uniqueID);
		requestJson[jsFields.kTransferPort] = serverTransferPort;
		string requestStr = requestJson.dump() + kCommandDelimiter;

		if (send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0) == 0)
		{
			return false;
		}

		{
			scoped_lock lock(sessionIDToNickNameMapMtx, userNickNamesSetMtx);
			sessionIDToNickNameMap[uniqueID] = (responseJson.value(jsFields.kNickname, jsFieldsDefault.kNickname) == jsFieldsDefault.kNickname) ? kDefaultDir : string(responseJson.value(jsFields.kNickname, ""));
			userNickNamesSet.insert(curNickName);
		}
		
		return true;
	}

	void acceptNewTransferConnection(/*const int uniqueID*/)  // if I accept multiple clients on transfer port in multiple threads, I need a mutex and a notifier to notify the waiting thread that the client has connected or that the timeou thas passed
	{
		SOCKET clientTransferSocket = INVALID_SOCKET;
	
		while (!ifExit.load())
		{
			clientTransferSocket = tryAcceptClientSocket(serverTransferSocket);
			
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

			bool ifWaiting = false;
			{
				lock_guard lock(sessionWaitingForTransferMtx);
				ifWaiting = sessionsWaitingForTransferConnection.contains(responseJson.value(jsFields.kUniqueID, kDefaultSessionID));
			}

			if (responseJson.value(jsFields.kStatusCode, common::StatusCode::kStatusFailure) != common::StatusCode::kStatusOK || 
				responseJson.value(jsFields.kUniqueID, kDefaultSessionID) == kDefaultSessionID || !ifWaiting
				/*!sessionsWaitingForTransferConnection.contains(responseJson.value(jsFields.kUniqueID, kDefaultSessionID))*/)
			{
				closesocket(clientTransferSocket);
				continue;
			}
			const int curClientID = responseJson[jsFields.kUniqueID];

			nlohmann::json serverJson = getServerJsonTemplate("", common::StatusCode::kStatusOK, curClientID);

			if (SOCKET_ERROR == send(clientTransferSocket, serverJson.dump().c_str(), serverJson.dump().size(), 0))
			{
				dropClient(curClientID);
				return;
			}

			{
				lock_guard lock(sessionWaitingForTransferMtx);
				sessionsWaitingForTransferConnection.erase(curClientID);
			}
			{
				lock_guard lock(sessionIDToTransferSocketMapMtx);
				sessionIDToClientTransferSocket[curClientID] = clientTransferSocket;
			}

		}

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

	bool initSockets()   
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

	static bool ifFileNameValid(const string fileName)
	{
		/*
		if (fileName.find("\\"))
		{
			return false;
		}
		*/
		return regex_match(fileName, validFileNamePattern);
	
	}

	SOCKET tryAcceptClientSocket(const SOCKET& serverSocket)
	{
		SOCKET clientSocket = INVALID_SOCKET;

		fd_set readfds;
		FD_ZERO(&readfds);  // Initialize the set to be empty
		FD_SET(serverSocket, &readfds);  // Add the listening socket to the set 
		TIMEVAL tv;
		tv.tv_sec = static_cast<long>(chrono::duration_cast<chrono::seconds>(kTimeout).count());
		tv.tv_usec = 0;
		int result = 0;

		result = select(0, &readfds, nullptr, nullptr, &tv);

		if (result > 0 && FD_ISSET(serverSocket, &readfds))
		{
			clientSocket = accept(serverSocket, nullptr, nullptr);
		}
		else if (result == SOCKET_ERROR)
		{
			cerr << format("Could not accept client at {}\n", __func__);
			return INVALID_SOCKET;
		}
		return clientSocket;
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

	void dropClient(const int curClientID)
	{
		scoped_lock compositeLock(curLoginedUsersSetMtx, sessionIDToControlSocketMapMtx, sessionIDToTransferSocketMapMtx, sessionIDToNickNameMapMtx, sessionWaitingForTransferMtx, alreadyUsedSessionIDSetMtx);

		closesocket(sessionIDToClientControlSocket[curClientID]);
		closesocket(sessionIDToClientTransferSocket[curClientID]);
		
		curLoginedUsersNamesSet.erase(sessionIDToNickNameMap[curClientID]);
		sessionIDToClientTransferSocket.erase(curClientID);
		sessionIDToClientControlSocket.erase(curClientID);
		sessionIDToNickNameMap.erase(curClientID);
		sessionsWaitingForTransferConnection.erase(curClientID);
		alreadyUsedIDSet.erase(curClientID);
	}


	bool loadConfig(ifstream& file)
	{
		if (!file.is_open())
		{
			cerr << "Error: File stream is not open.\n";
			return false;
		}
		if (common::getFileSize(file) == 0)
			return true;

		try
		{
			file >> configJson;
		}
		catch (const nlohmann::json::parse_error& e)
		{
			cerr << "JSON Parse Error: " << e.what() << "\n";
			return false;
		}

		return true;
	}


	inline void parseConfigCreateDir()
	{
		if (!configJson.contains(jsFields.kUsersNicknames)
			|| !configJson[jsFields.kUsersNicknames].is_array())
		{
			return;
		}
		for (string& name : configJson.value(jsFields.kUsersNicknames, vector<string>{}))
		{
			if (!filesystem::is_directory(name) && !fileExists(name))
			{
				cerr << format("Error at {}, could not find dir {}, create empty\n", __func__, name);
				filesystem::create_directories(name);
			}
			userNickNamesSet.insert(name);
		}
	}

	static inline bool fileExists( string filePath)
	{
		//filePath = "." + filePath;
		return filesystem::exists(filePath) && filesystem::is_regular_file(filePath);
	}

	const string kConfigName = "";
	nlohmann::json configJson;


	static inline const int kDefaultSessionID = -1;
		
	static inline const int kUniqueIdUpBound = 65535;
	static inline const string kDefaultDir = "default";
	static inline regex validFileNamePattern = basic_regex<char>(R"(^[a-zA-Z0-9_]+(\.[a-zA-Z0-9_]+)?$)");
	static inline chrono::seconds kTimeout{ 2 };
	static inline const common::JsonFields jsFields;
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
	static inline sockaddr_in serverAddr;
	static inline atomic<bool> ifExit = false ;
	static inline int serverControlPort;
	static inline int serverTransferPort;

	static inline shared_ptr<ServerTcp> /*ServerTcp* */thisServerPtr = nullptr;
	static inline shared_ptr<thread> controlListeningThreadPtr = nullptr;
	static inline shared_ptr<thread> transferListeningThreadPtr = nullptr;

	static inline SOCKET serverControlSocket = INVALID_SOCKET;
	static inline SOCKET serverTransferSocket = INVALID_SOCKET;

	mutex sessionIDToControlSocketMapMtx, sessionIDToTransferSocketMapMtx, sessionIDToNickNameMapMtx, sessionWaitingForTransferMtx, curLoginedUsersSetMtx, userNickNamesSetMtx, alreadyUsedSessionIDSetMtx;


	static inline unordered_map<int, SOCKET> sessionIDToClientControlSocket;
	static inline unordered_map<int, SOCKET> sessionIDToClientTransferSocket;
	//static inline unordered_map<string, int> userNamesToSessionId;
	static inline unordered_map<int, string> sessionIDToNickNameMap;

	static inline unordered_set<int> sessionsWaitingForTransferConnection;
	static inline unordered_set<string> curLoginedUsersNamesSet;
	static inline unordered_set<string> userNickNamesSet;
	static inline unordered_set<long long> alreadyUsedIDSet;

	vector<thread> clientsThreadVec;	
};

int main()
{
	
	auto result = ServerTcp::initServer("config.txt");
	
	if (result.has_value())
	{
		shared_ptr<ServerTcp> ptr = result.value();

		ptr->bindSockets(1234, 1235);
		ptr->startListening();
		this_thread::sleep_for(18923s);
		ptr.~shared_ptr();  // without this line there is an exception ans select() does not stop in acceptConnection()

	}
	 


	return 0;

}
