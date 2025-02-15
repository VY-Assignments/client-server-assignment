#include <iostream>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <vector>
#include <tuple>
#include <optional>
#include <conio.h>
#include <format>
#include <thread>
#include <fstream>
#include <regex>
#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")
using namespace std;

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

enum class Command : int
{
	kGet = 0,
	kList,
	kPut,
	kDelete,
	kInfo,
	kDefault,
	kExit

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
	const string kMessage		= "message";
	const string kStatusCode	= "statusCode";
	const string kTransferPort  = "transferPort";
	const string kCommand		= "command";
	const string kArgument		= "argument";
	const string kVersion		= "version";
	const string kUniqueID		= "uniqueID";
	const string kFileSize		= "fileSize";
	const string kFileNames		= "fileNames";
	const string kFileInfo		= "fileInfo";
};


class ClientTcp
{
public:

	static optional<shared_ptr<ClientTcp>> initClient()
	{
		if (thisClientPtr != nullptr)
		{
			return thisClientPtr;
		}

		int result = 0;

		if (!isLibLoaded)
		{
			result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		}
		
		if (result != 0)
		{
			return std::nullopt;
		}

		isLibLoaded = true;
		thisClientPtr = shared_ptr<ClientTcp>(new ClientTcp());

		return thisClientPtr;
	}
	
	bool initSocket() noexcept
	{
		clientControlSocket = socket(AF_INET, SOCK_STREAM, 0);
		clientTransferSocket = socket(AF_INET, SOCK_STREAM, 0);

		if (clientControlSocket == INVALID_SOCKET || clientTransferSocket == INVALID_SOCKET)
		{
			cerr << format("Error ar {}, could not init sockets\n", __func__);
			return false;
		}
		return true;
	}
	
	bool tryConnectToServer(const int serverControlPort, const PCWSTR serverIP) noexcept  // extend it so it would also init the additional server socket
	{
		if (isConnected)
		{
			cerr << format("Error at {}, could not connect, already connected\n", __func__);
			return false;
		}
		if (clientControlSocket == INVALID_SOCKET && !initSocket())
		{
			cerr << format("Error ar {}, could not init socket object, error: {}\n", __func__, WSAGetLastError());
			return false;	
		}


		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(serverControlPort);
		InetPton(AF_INET, serverIP, &serverAddr.sin_addr);

		if (connect(clientControlSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
		{
			cerr << format("Error at {}, connection failed with error: {}\n", __func__, WSAGetLastError());
			isConnected = false;
			dropAllConnections();
			return false;
		}

		isConnected = true;
		
		if (!tryHandShake())
		{
			cerr << format("Error at {}, server does not follow handshake\n", __func__);
			isConnected = false;
			dropAllConnections();
			return false;
		}

		if (!tryConnectToTransferPort())
		{
			cerr << format("Error at {}, could not connect to transfer port #{}\n", __func__, serverTransferPort);
			isConnected = false;
			dropAllConnections();
			return false;
		}

		wcout << format(L"SUCCESS, connected to {}:{}, TransferPort: {}\n", serverIP, serverControlPort, serverTransferPort);
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
		
		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		requestJson[jsFields.kMessage] = kClientHandShakePhrase;
		requestJson[jsFields.kVersion] = kClientVersion;

		string requestStr = requestJson.dump() + kCommandDelimiter;
		
		for (int index = 0; index < 2; index++)
		{
			memset(buffer, 0, sizeof(buffer));
			bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0) > 0;
			
			if (bytesReceived == 0)
			{
				dropAllConnections();
				cout << format("Error at {}, server dropped connection\n", __func__);
				return false;
			}
			
			string response = buffer;
			responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));

			if (index == 0) 
			{
				if (!responseJson.contains(jsFields.kMessage) || 
					!responseJson.contains(jsFields.kStatusCode) ||
					responseJson[jsFields.kMessage] != kServerHandShakePhrase)
				{
					cerr << format("Error at {}, wrong response\n", __func__);
					return false;
				}
				
				send(clientControlSocket, requestStr.c_str(), (int)requestStr.size(), 0);
			}
			else if (index == 1)
			{
				if (!responseJson.contains(jsFields.kStatusCode) || 
					!responseJson.contains(jsFields.kTransferPort) || 
					!responseJson.contains(jsFields.kUniqueID) ||
					responseJson[jsFields.kStatusCode] != StatusCode::kStatusOK)
				{
					cerr << format("Error at{}, status code was {}\n", __func__, responseJson.value(jsFields.kStatusCode, "Unknown"));
					return false;
				}
			}
		}

		serverTransferPort = responseJson[jsFields.kTransferPort];
		clientID = responseJson[jsFields.kUniqueID];

		return true;
	}
	 
	bool tryConnectToTransferPort()
	{
		short retryCounter = 0, bytesReceived = 0; 

		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));
		
		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		requestJson[jsFields.kUniqueID] = clientID;
		requestJson[jsFields.kStatusCode] = StatusCode::kStatusOK;
		string responseStr, requestStr = requestJson.dump();

		serverAddr.sin_port = htons(serverTransferPort);
		InetPton(AF_INET, serverIP, &serverAddr.sin_addr);

		while (connect(clientTransferSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR && retryCounter <= kMaxRetryCounter)
		{
			cerr << format("Error at {}, connection to transfer port failed with error: {}, Retrying...\n", __func__, WSAGetLastError());
			this_thread::sleep_for(100ms);	
			retryCounter++;
			if (retryCounter == kMaxRetryCounter)
			{
				return false;
			}
		}

		send(clientTransferSocket, requestStr.c_str(), requestStr.size(), 0);

		bytesReceived = recv(clientTransferSocket, buffer, (int)sizeof(buffer) - 1, 0); 

		if(bytesReceived == 0)
		{
			cerr << format("Error at {}, received 0 bytes, closing connection\n", __func__);
			dropAllConnections();
			return false;
		}
		
		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));
		
		if (responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure) != StatusCode::kStatusOK)
		{
			cerr << format("Error ar {}, the status code was {}\n", __func__, responseJson.value(jsFields.kStatusCode, "Unknown"));
			return false;
		}

		return true;
	}

	bool getFile(string fileName)
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return false;
		}
		if (fileName.size() > kMaxFileNameSize || fileName.size() < kMinFileNameSize)
		{
			cerr << format("Error at {}, filename of a wrong lenght\n", __func__);
			return false;
		}

		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		int bytesReceived = 0;

		requestJson[jsFields.kCommand] = Command::kGet;
		requestJson[jsFields.kArgument] = fileName;
		requestJson[jsFields.kUniqueID] = clientID;

		string requestStr = requestJson.dump() + kCommandDelimiter;

		send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0);
		
		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);

		if (bytesReceived == 0)
		{
			cerr << format("Error at {}, server dropped connection, dropping connection\n", __func__);
			dropAllConnections();
			return false;
		}
		
		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));

		if (!responseJson.contains(jsFields.kStatusCode) || !responseJson.contains(jsFields.kFileSize))
		{
			cerr << format("Error at {}, status code or file size is Unknown\n", __func__);
			return false;
		}
		switch (responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure))
		{
		case StatusCode::kStatusOK:
			cout << format("Starting transfering file {}\n", fileName);
			break;
		case StatusCode::kStatusNotFound:
			cerr << format("Error at {}, file not found {}", __func__, string(responseJson[jsFields.kStatusCode]));
			return false;
		case StatusCode::kStatusFailure:
			cerr << format("Error at {}, server error, message {}", __func__, string(responseJson[jsFields.kMessage]));
			return false;
		default: 
			cerr << format("Error at {}, unexpected status code\n", __func__);
			return false;
		}
		
		if (!acceptFile(fileName, responseJson[jsFields.kFileSize]))
		{
			cerr << format("Error ar {}, could not accpet the file\n", __func__);
			return false;
		}
		cout << format("Success, file {} accepted\n", fileName);
		
		return true;
	}

	bool deleteFileRemote(string fileName) 
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return false;
		}
		if (fileName.size() > kMaxFileNameSize || fileName.size() < kMinFileNameSize)
		{
			cerr << format("Error at {}, filename of a wrong length\n", __func__);
			return false;
		}
		int bytesReceived = 0;
		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		
		requestJson[jsFields.kCommand] = Command::kDelete;
		requestJson[jsFields.kArgument] = fileName;

		string requestStr = requestJson.dump() + kCommandDelimiter;

		send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0);
		
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);
		
		if (bytesReceived == 0)
		{
			cerr << format("Error at {}, server droppped connection, closing\n", __func__);
			dropAllConnections();
			return false;
		}

		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));
		
		if (!responseJson.contains(jsFields.kStatusCode))
		{
			cerr << format("Error ar {}, status code Unknown\n", __func__);
			return false;
		}

		switch (responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure))
		{
		case StatusCode::kStatusOK:
			cout << format("Deleted successfully\n");
			return true;
		case StatusCode::kStatusNotFound:
			cout << format("File {} not found on server\n", fileName);
			return false;
		default:
			cerr << format("Error at {}, status code was {}\n", __func__, string(responseJson[jsFields.kStatusCode]));
			break;
		}

		return false;
	}

	bool putFile(string fileName)
	{
		ifstream fileToUpload(fileName, ios::binary);

		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return false;
		}
		if (fileName.size() > kMaxFileNameSize || fileName.size() < kMinFileNameSize)
		{
			cerr << format("Error at {}, filename of a wrong length\n", __func__);
			return false;
		}
		if (!fileToUpload)
		{
			cout << format("Could not open the file {}\n", fileName);
			return false;
		}

		int bytesReceived = 0;
		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		requestJson[jsFields.kCommand] = Command::kPut;
		requestJson[jsFields.kArgument] = fileName;
		requestJson[jsFields.kFileSize] = getFileSize(fileToUpload);

		string requestStr = requestJson.dump() + kCommandDelimiter;

		send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0);

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);

		if (bytesReceived == 0)
		{
			cerr << format("Error at {}, server disconnected\n", __func__);
			return false;
		}
		
		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));

		if (!responseJson.contains(jsFields.kStatusCode))
		{
			cerr << format("Error at {}, status code was Unknown\n", __func__);
			return false;
		}

		switch (responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure))
		{
		case StatusCode::kStatusOK:
			cout << "Start uploading:\n";
			break;
		default:
			cout << format("The status code was not OK, message: {}\n", responseJson.value(jsFields.kMessage, "_"));
			return false;
		}

		if (uploadFile(fileToUpload))
		{
			cout << format("Success, file {} uploaded\n", fileName);
			return true;
		}
		cout << format("Could not upload the file {}\n", fileName); 
		
		return false;
	}

	bool listCurDir()
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return false;
		}
		
		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		requestJson[jsFields.kCommand] = Command::kList;

		string requestStr = requestJson.dump() + kCommandDelimiter;

		size_t bytesSent = 0, bytesReceived = 0, listSize = 0;
	
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));
	
		bytesSent = send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0);

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);

		if (bytesReceived == 0 || bytesSent == SOCKET_ERROR)  
		{
			cerr << format("Error at {}, server disconnected, last error {}, closing...\n", __func__, WSAGetLastError());
			dropAllConnections();
			return false;
		}
		
		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));
		
		if (!responseJson.contains(jsFields.kStatusCode) || !responseJson.contains(jsFields.kFileSize))
		{
			cerr << format("Error at {}, status code was {}", __func__, responseJson.value(jsFields.kStatusCode, static_cast<int>(StatusCode::kStatusFailure)));
			return false;
		}
		
		listSize = responseJson[jsFields.kFileSize];
		
		nlohmann::json info = loadDirInfo(listSize);

		printDirList(info);

		return true;
	}

	bool getFileInfo(const string fileName) 
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return false;
		}

		nlohmann::json responseJson, requestJson = getClientJsonTemplate();
		requestJson[jsFields.kCommand] = Command::kInfo;
		requestJson[jsFields.kArgument] = fileName;

		string requestStr = requestJson.dump() + kCommandDelimiter;

		size_t bytesSent = 0, bytesReceived = 0, listSize = 0;

		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));

		bytesSent = send(clientControlSocket, requestStr.c_str(), requestStr.size(), 0);

		bytesReceived = recv(clientControlSocket, buffer, sizeof(buffer) - 1, 0);

		if (bytesReceived == 0 || bytesSent == SOCKET_ERROR)
		{
			cerr << format("Error at {}, server disconnected, last error {}, closing...\n", __func__, WSAGetLastError());
			dropAllConnections();
			return false;
		}

		string response = buffer;
		responseJson = nlohmann::json::parse(response.substr(0, response.find(kCommandDelimiter)));

		if (!responseJson.contains(jsFields.kStatusCode) || responseJson.value(jsFields.kStatusCode, StatusCode::kStatusFailure) != StatusCode::kStatusOK)
		{
			cerr << format("Status code was {}, message: {}\n", responseJson.value(jsFields.kStatusCode, static_cast<int>(StatusCode::kStatusFailure)), responseJson.value(jsFields.kMessage, ""));
			return false;
		}

		printFileInfo(fileName, loadFileInfo());

		return true;
	}

	~ClientTcp()
	{
		dropAllConnections();
		if (isLibLoaded)
		{
			WSACleanup();
		}
	}
private:
	ClientTcp() {}


	static inline const size_t getFileSize(ifstream& file)  
	{
		streampos current = file.tellg();  
		file.seekg(0, std::ios::end);           
		size_t size = file.tellg();             
		file.seekg(current, std::ios::beg);     

		return size;
	}


	static inline void printDirList(nlohmann::json listJsonIn) 
	{
		int counter = 0;
		for (auto& fileName : listJsonIn[jsFields.kFileNames])
		{
			counter++;
			cout << fileName << "\t";
			if (counter % 2 == 0)	cout << endl;
		}
		if (counter % 2 != 0)
			cout << endl;
	}
	static inline void printFileInfo(const string& fileName, const nlohmann::json& fileInfo)
	{
		cout << format("{} {}\n", fileName, fileInfo.value(jsFields.kFileInfo, ""));
	}

	nlohmann::json loadFileInfo()
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return nlohmann::json{};
		}
		const int kChunkSize = 1024;

		char buffer[kChunkSize];
		memset(buffer, 0, sizeof(buffer));

		nlohmann::json responseJson;
		string resultStr;

		int bytesReceived = 0, totalBytesReceived = 0;

		bytesReceived = recv(clientTransferSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytesReceived == 0)
		{
			cerr << format("Error at {}, server disconnected, closing\n", __func__);
			dropAllConnections();
			return nlohmann::json{};
		}

		return nlohmann::json::parse(buffer);
	}


	nlohmann::json loadDirInfo(const size_t listSize)
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, server is not connected\n", __func__);
			dropAllConnections();
			return nlohmann::json{};
		}
		const size_t chunkSize = 1024;
		
		char buffer[chunkSize];
		memset(buffer, 0, sizeof(buffer));
		
		int bytesReceived = 0, totalBytesReceived = 0;

		string resultListStr;

		while (totalBytesReceived < static_cast<int>(listSize) && (bytesReceived = recv(clientTransferSocket, buffer, sizeof(buffer) - 1, 0)) && bytesReceived != 0)
		{
			resultListStr += buffer;
			totalBytesReceived += bytesReceived;
			memset(buffer, 0, sizeof(buffer));
		}
		
		return nlohmann::json::parse(resultListStr);
	}

	


	void dropAllConnections()
	{
		closesocket(clientControlSocket);
		closesocket(clientTransferSocket);

		clientControlSocket = INVALID_SOCKET;
		clientTransferSocket = INVALID_SOCKET;
		
		isConnected = false;
	}
	bool isConnectionValid() const noexcept
	{
		return clientControlSocket != INVALID_SOCKET && clientTransferSocket != INVALID_SOCKET && isConnected;
	}
	bool acceptFile(const string& fileName, const int fileSize) const
	{
		if (!isConnectionValid())
		{
			cerr << format("Error at {}, connection was not valid\n", __func__);
			return false;
		}
		int bytesReceived = 0, totalRealBytesReceived = 0;

		char buffer[1024];

		ofstream newFile(fileName, ios::binary);
		
		while(totalRealBytesReceived < fileSize && (bytesReceived = recv(clientTransferSocket, buffer, sizeof(buffer) - 1, 0)) && bytesReceived != 0)
		{
			// string decodedStr = base64_decode(buffer);

			newFile.write(buffer, bytesReceived);
			totalRealBytesReceived += bytesReceived;
		}
		newFile.close();

		return true; 
	}
	
	bool uploadFile(ifstream& file)    
	{
		if (!isConnected)
		{
			cerr << format("Error at {}, connection was not valid, closing connections\n", __func__);
			dropAllConnections();
			return false;
		}

		const size_t kChunkSize = 1024, kFileSize = getFileSize(file);
		int bytesRead = 0, totalRealBytesUploaded = 0;
		
		char buffer[kChunkSize]; 
		memset(buffer, 0, sizeof(buffer));

		while (file.read(buffer, kChunkSize) || file.gcount() > 0) 
		{	
			bytesRead = file.gcount();  
			totalRealBytesUploaded += bytesRead;
			
			// string encoded = base64_encode(string(buffer, bytesRead));

			if (SOCKET_ERROR == send(clientTransferSocket, buffer, bytesRead, 0))   // raw 
			{ 
				cerr << format("Error uploading file, aborting\n");
				return false;
			}
			memset(buffer, 0, sizeof(buffer));
		}

		return true;
	}
	
	nlohmann::json getClientJsonTemplate() const
	{
		return nlohmann::json{
			{jsFields.kMessage , ""},
			{jsFields.kCommand, ""},
			{jsFields.kArgument, ""},
			{jsFields.kUniqueID, clientID},
			{jsFields.kVersion, kClientVersion}};
	}


	sockaddr_in serverAddr;

	static inline bool isLibLoaded = false;
	static inline WSADATA wsaData;

	static inline const string kServerHandShakePhrase = "Hello client";
	static inline const string kClientHandShakePhrase = "Hello server";
	static inline const string kServerConfirmationPhrase = "OK";
	static inline const string kCommandDelimiter		 = "||";
	static inline const int	   kClientVersion			 = 1;
	static inline const int	   kMaxRetryCounter			 = 4;
	static inline const int	   kMaxFileNameSize			 = 15;
	static inline const int    kMinFileNameSize			 = 1;
	static inline shared_ptr<ClientTcp>	thisClientPtr	 = nullptr;


	static inline const JsonFields jsFields;


	

	bool isConnected = false;
	int serverControlPort = 0;
	int serverTransferPort = 0;
	int clientID = -1;
	PCWSTR serverIP;

	static inline SOCKET clientControlSocket  = INVALID_SOCKET;
	static inline SOCKET clientTransferSocket = INVALID_SOCKET;
	
	friend class shared_ptr<ClientTcp>;
};

class Program
{
private:
	shared_ptr<ClientTcp> client;

public:
	void start()
	{
		int port;
		wstring serverIP;
		string name;

		cout << "Enter server IP: ";
		wcin >> serverIP;
		cout << "Enter server control port: ";
		cin >> port;

		auto clientOpt = ClientTcp::initClient();
		if (!clientOpt)
		{
			cerr << "Failed to initialize client.\n";
			return;
		}
		client = clientOpt.value();

		if (!client->tryConnectToServer(port, serverIP.c_str()))
		{
			return;
		}

		string input;
		getline(cin, input); // to get rid of \n

		promptCommands();
	}

private:
	void promptCommands()
	{
		string input;

		while (true)
		{
			cout << "> ";
			getline(cin, input);

			input = input.substr(0, input.find('\n'));

			Command command = handleCommand(input);
			if (command == Command::kDefault)
			{
				cout << "Unknown command. Available commands: put, get, delete, info, list, exit.\n";
				continue;
			}

			if (command == Command::kExit)
				break;

			executeCommand(command, input);
		}
	}

	Command handleCommand(const string& input)
	{

		if (regex_match(input, putPattern))
			return Command::kPut;

		else if (regex_match(input, getPattern))
			return Command::kGet;

		else if (regex_match(input, deletePattern))
			return Command::kDelete;

		else if (regex_match(input, infoPattern))
			return Command::kInfo;

		else if (regex_match(input, listPattern))
			return Command::kList;

		else if (input == "exit")
			return Command::kExit;  // Handle exit command


		return Command::kDefault;  // Default for unrecognized commands
	}

	void executeCommand(const Command command, const string& input)
	{

		switch (command)
		{
		case Command::kPut:
		{
			smatch match;
			regex_search(input, match, regex(R"(^put\s+(.+)$)"));
			client->putFile(match[1]);
			break;
		}
		case Command::kGet:
		{
			smatch match;
			regex_search(input, match, regex(R"(^get\s+(.+)$)"));
			client->getFile(match[1]);
			break;
		}
		case Command::kDelete:
		{
			smatch match;
			regex_search(input, match, regex(R"(^delete\s+(.+)$)"));
			client->deleteFileRemote(match[1]);
			break;
		}
		case Command::kInfo:
		{
			smatch match;
			regex_search(input, match, regex(R"(^info\s+(.+)$)"));
			client->getFileInfo(match[1]);
			break;
		}
		case Command::kList:
			client->listCurDir();
			break;
		default:
			cout << "Invalid command.\n";
			break;
		}
	}

	static inline const regex putPattern = basic_regex<char>(R"(^put\s+(.+)$)");
	static inline const regex getPattern = basic_regex<char>(R"(^get\s+(.+)$)");
	static inline const regex deletePattern = basic_regex<char>(R"(^delete\s+(.+)$)");
	static inline const regex infoPattern = basic_regex<char>(R"(^info\s+(.+)$)");
	static inline const regex listPattern = basic_regex<char>(R"(^list\s*$)");

};




int main()
{

	Program prog;
	prog.start();


	/*
	auto result = ClientTcp::initClient();

	shared_ptr<ClientTcp> ptr = result.value();

	ptr->initSocket();
	if (ptr->tryConnectToServer(1234, L"127.0.0.1"))
	{
		cout << "SUCCESS, connected\n";
	};
	if (ptr->putFile("abab.jpg"))
	{
		cout << "success put file\n";
	}
	if (ptr->getFileInfo("abab.jpg"))
	{
		cout << "success info\n";
	}
	
	// ptr->listCurDir();
	ptr.~shared_ptr();
	*/


	return 0;

}


