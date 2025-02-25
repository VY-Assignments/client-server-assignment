#pragma once
#include <fstream>
#include <string>

namespace common
{
	enum class Command : size_t
	{
		kGet = 0,
		kList,
		kPut,
		kDelete,
		kInfo,
		kDefault, 
		kExit
	};

	enum class StatusCode : size_t
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

	struct JsonFields
	{
		const std::string kMessage		 = "message";
		const std::string kStatusCode	 = "statusCode";
		const std::string kTransferPort	 = "transferPort";
		const std::string kCommand		 = "command";
		const std::string kArgument		 = "argument";
		const std::string kVersion		 = "version";
		const std::string kUniqueID		 = "uniqueID";
		const std::string kFileSize		 = "fileSize";
		const std::string kFileNames	 = "fileNames";
		const std::string kFileInfo		 = "fileInfo";
		const std::string kNickname		 = "nickname";
		const std::string kUsersNicknames = "user_nicknames";
	};
}