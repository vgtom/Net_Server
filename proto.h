#ifndef _PROTO_H
#define _PROTO_H

#include <string>
#include <istream>
#include <sstream>
#include <fstream>
#include <iostream>
#include <vector>

using namespace std;

class Proto
{
public:
	Proto() {
	}

	Proto(std::string& data){
		m_stream = MakeStream(data);;
	}

	Proto& operator=(std::string& data) {
		m_stream = MakeStream(data);
		return *this;
	}

	Proto& operator=(char* data) {
		std::string strT = data;
		m_stream = MakeStream(strT);
		return *this;
	}

	Proto& operator=(const Proto& obj) {
		if (this != &obj) {
			m_stream = obj.Stream();
		}
	}

	std::string Stream() const{
		return m_stream;
	}

	int Size() {
		return m_len;
	}

	std::string Data() {
		return m_data;
	}

	bool Parse(std::string& strm) {
		istringstream vstr(strm);
		std::string str;
		std::getline(vstr, str);
		istringstream vstr1(str);
		string str1, str2;
		vstr1 >> str1 >> str2;
		if (str1.compare("DATA") != 0)
		{
			return false;
		}
		int len = atoi(str2.c_str());

		strm.replace(0, str.size() + 2,"");

		m_data = strm;
		m_len = len;

		return true;
	}

	bool IsAllDataReceived(){
		return m_len == m_data.size();
	}
	void AddToData(char* data) {
		m_data += data;
	}

private:
	std::string MakeStream(std::string& data) {
		string strT = "DATA ";
		int len = data.size();
		m_len = len;

		char buf[10];
#ifdef WIN32
		_itoa_s(len, buf, 10);
#elif UNIX
		itoa(len, buf, 10);
#endif
		strT += buf;

		strT += "\n\n";
		strT += data;

		return strT;
	}

	std::string m_stream;
	int m_len;
	std::string m_data;
};

class ServerManager {
private:
	struct ServerToPort {
		string server;
		int port;
	};
public:
	ServerManager() {}
	ServerManager(char* fileName) {
		ifstream  infile(fileName);

		std::string line;
		while (std::getline(infile, line))
		{
			std::istringstream iss(line);
			string server;
			int port;
			if (!(iss >> server >> port)) {
				break;
			} // error

			ServerToPort obj = { server,port };
			aServerToPort.push_back(obj);
		}

		m_Current = 0;
	}

	void SelectServer(void) {
		printf("List of Servers are below \n");
		printf("--------------------------------------------------------------\n");
			
		std::vector<ServerToPort>::iterator it = aServerToPort.begin();
		int Cnt = 1;
		for (; it != aServerToPort.end(); it++) {
			ServerToPort obj = *it;
			printf("%d)...%s...%d \n", Cnt, obj.server.c_str(),obj.port);
			Cnt++;
		}

		printf("Make an entry(number) to select a server/port combination from below \n");
		
		int num;
#ifdef WIN32
		scanf_s("%d", &num);
#elif UNIX
		scanf("%d", &num);
#endif
	
		m_Current = num-1;
	}

	std::string GetServer() {
		return aServerToPort[m_Current].server;
	}
	int GetPort() {
		return aServerToPort[m_Current].port;
	}

private:
	std::vector<ServerToPort> aServerToPort;
	int m_Current;
};

class MessageFile {
public:
	MessageFile(char* fileName) {
		m_fileName = fileName;
		m_bOpen = false;
		
	}
	void Open() {
		m_infile.open(m_fileName);
		m_bOpen = true;
	}
	string GetLine() {
		if (!m_bOpen)
			return "";

		std::string line;
		std::getline(m_infile, line);

		return line;
	}

	void Close() {
		m_infile.close();
		m_bOpen = false;
	}
private:
	string m_fileName;
	ifstream m_infile;
	bool m_bOpen;
};

#endif