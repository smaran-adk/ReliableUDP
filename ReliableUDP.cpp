/*
*
*	This file is used in order to run the reliableUDP application. Depending on if command-line arguments are used,
*	the program will either be ran as a Client or a Server. The client is used to send an ASCII file to the server
*	through a UDP protocol.
*/


/*
	Reliability and Flow Control Example
	From "Networking for Game Programmers" - http://www.gaffer.org/networking-for-game-programmers
	Author: Glenn Fiedler <gaffer@gaffer.org>
*/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "Net.h"

std::vector<unsigned char> readFileIntoVector(const std::string& filename);
void readVectorToCharArray(const std::vector<unsigned char>& data, unsigned char* output, std::size_t startIndex);
void writeCharArrayToFile(const char* filename, const unsigned char* data, std::size_t dataSize);

//#define SHOW_ACKS

using namespace std;
using namespace net;


// Constants for better readability
/*
* Magic Numbers can be replaced for the better maintainability
*/
const int ServerPort = 30000;
const int ClientPort = 30001;
const int ProtocolId = 0x11223344;
const float DeltaTime = 1.0f / 30.0f;
const float SendRate = 1.0f / 30.0f;
const float TimeOut = 10.0f;
const int PacketSize = 256;

class FlowControl
{
public:

	FlowControl()
	{
		printf("flow control initialized\n");
		Reset();
	}

	void Reset()
	{
		mode = Bad;
		penalty_time = 4.0f;
		good_conditions_time = 0.0f;
		penalty_reduction_accumulator = 0.0f;
	}

	void Update(float deltaTime, float rtt)
	{
		const float RTT_Threshold = 250.0f;

		if (mode == Good)
		{
			if (rtt > RTT_Threshold)
			{
				printf("*** dropping to bad mode ***\n");
				mode = Bad;
				if (good_conditions_time < 10.0f && penalty_time < 60.0f)
				{
					penalty_time *= 2.0f;
					if (penalty_time > 60.0f)
						penalty_time = 60.0f;
					printf("penalty time increased to %.1f\n", penalty_time);
				}
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				return;
			}

			good_conditions_time += deltaTime;
			penalty_reduction_accumulator += deltaTime;

			if (penalty_reduction_accumulator > 10.0f && penalty_time > 1.0f)
			{
				penalty_time /= 2.0f;
				if (penalty_time < 1.0f)
					penalty_time = 1.0f;
				printf("penalty time reduced to %.1f\n", penalty_time);
				penalty_reduction_accumulator = 0.0f;
			}
		}

		if (mode == Bad)
		{
			if (rtt <= RTT_Threshold)
				good_conditions_time += deltaTime;
			else
				good_conditions_time = 0.0f;

			if (good_conditions_time > penalty_time)
			{
				printf("*** upgrading to good mode ***\n");
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				mode = Good;
				return;
			}
		}
	}

	float GetSendRate()
	{
		// adjust send rate based on the current mode
		return mode == Good ? 30.0f : 10.0f;
	}

private:
	// Enumeration for managing flow control states
	enum Mode
	{
		Good,
		Bad
	};

	Mode mode;
	float penalty_time;
	float good_conditions_time;
	float penalty_reduction_accumulator;
};

//  Main function body ----------------------------------------------

/*
separate the Client and server tasks
*/
int main(int argc, char* argv[])
{
	// parse command line

	enum Mode
	{
		Client,
		Server
	};

	Mode mode = Server;
	Address address;

	if (argc >= 2)
	{
		int a, b, c, d;
		// retrieve additional command line arguments to determine the mode and addresses	

#pragma warning(supress:4996)
		if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
		{
			mode = Client;
			address = Address(a, b, c, d, ServerPort);
		}
	}
	/*
	allocating sockets for sending metadata allocation from a reliable connection
	*/

	// initialzing  sockets in this parts

	char* sendingFile = NULL;
	std::vector<unsigned char> binaryContent;
	size_t startingIndex = 0;

	if (argc == 3 && mode == Client)
	{
		sendingFile = argv[2];

		ifstream file;



		file.open(sendingFile);



		if (!file)
		{
			printf("The file does not exist\n");
			return 0;
		}

		file.close();

		binaryContent = readFileIntoVector(sendingFile);
	}
	if (!InitializeSockets())
	{
		printf("failed to initialize sockets\n");
		return 1;
	}
	// create a reliable connection

	ReliableConnection connection(ProtocolId, TimeOut);

	// determine the port based on mode
	const int port = mode == Server ? ServerPort : ClientPort;

	//start the connection on the specific port
	if (!connection.Start(port))
	{
		printf("could not start connection on port %d\n", port);
		return 1;
	}

	if (mode == Client)
		connection.Connect(address);

	else
		connection.Listen();

	bool connected = false;
	float sendAccumulator = 0.0f;
	float statsAccumulator = 0.0f;

	FlowControl flowControl;

	while (true)
	{
		// update flow control

		if (connection.IsConnected())
			flowControl.Update(DeltaTime, connection.GetReliabilitySystem().GetRoundTripTime() * 1000.0f);

		const float sendRate = flowControl.GetSendRate();

		// detect changes in connection state

		if (mode == Server && connected && !connection.IsConnected())
		{
			flowControl.Reset();
			printf("reset flow control\n");
			connected = false;
		}

		if (!connected && connection.IsConnected())
		{
			printf("client connected to server\n");
			connected = true;
		}

		if (!connected && connection.ConnectFailed())
		{
			printf("connection failed\n");
			break;
		}
		//server task
		// send and receive packets

		sendAccumulator += DeltaTime;
		
		//send packets at the specific rate
		while (sendAccumulator > 1.0f / sendRate)
		{
			unsigned char packet[PacketSize];

			// If the program is being run as a client, read the vector for the file content and send it as a packet

			if (mode == Client)
			{
				readVectorToCharArray(binaryContent, packet, startingIndex);
			}
			memset(packet, 0, sizeof(packet));
			connection.SendPacket(packet, sizeof(packet));
			sendAccumulator -= 1.0f / sendRate;
		}

		while (true)
		{
			unsigned char packet[256];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));

			// If any content was in the packet, write the content to a file (otherwise, break from the loop)

			if (bytes_read > 0)
			{
				writeCharArrayToFile("output.txt", packet, sizeof(packet));
			}

			else if (bytes_read == 0)
				break;
		}

		// show packets that were acked this frame

#ifdef SHOW_ACKS
		unsigned int* acks = NULL;
		int ack_count = 0;
		connection.GetReliabilitySystem().GetAcks(&acks, ack_count);
		if (ack_count > 0)
		{
			printf("acks: %d", acks[0]);
			for (int i = 1; i < ack_count; ++i)
				printf(",%d", acks[i]);
			printf("\n");
		}
#endif

		// update connection

		connection.Update(DeltaTime);

		// show connection stats

		statsAccumulator += DeltaTime;
		//display connection statistics at intervals
		/*
		verifying the file integrity and writing the pieces of the disk file intervals
		*/
		while (statsAccumulator >= 0.25f && connection.IsConnected())
		{
			float rtt = connection.GetReliabilitySystem().GetRoundTripTime();

			unsigned int sent_packets = connection.GetReliabilitySystem().GetSentPackets();
			unsigned int acked_packets = connection.GetReliabilitySystem().GetAckedPackets();
			unsigned int lost_packets = connection.GetReliabilitySystem().GetLostPackets();

			float sent_bandwidth = connection.GetReliabilitySystem().GetSentBandwidth();
			float acked_bandwidth = connection.GetReliabilitySystem().GetAckedBandwidth();

			printf("rtt %.1fms, sent %d, acked %d, lost %d (%.1f%%), sent bandwidth = %.1fkbps, acked bandwidth = %.1fkbps\n",
				rtt * 1000.0f, sent_packets, acked_packets, lost_packets,
				sent_packets > 0.0f ? (float)lost_packets / (float)sent_packets * 100.0f : 0.0f,
				sent_bandwidth, acked_bandwidth);

			statsAccumulator -= 0.25f;
		}

		net::wait(DeltaTime);
	}

	ShutdownSockets();

	return 0;
}
std::vector<unsigned char> readFileIntoVector(const std::string& fileName)
{
	std::ifstream file(fileName, std::ios::binary);

	if (!file) {
		printf("ERROR: Could not open file...\n");
		return {};
	}

	// Get the file size
	file.seekg(0, std::ios::end);
	std::size_t fileSize = file.tellg();
	file.seekg(0, std::ios::beg);

	// Read the entire file into a vector
	std::vector<unsigned char> fileContents(fileSize);
	file.read(reinterpret_cast<char*>(fileContents.data()), fileSize);

	if (!file) {
		printf("ERROR: File read error");
		return {};
	}

	file.close();
	return fileContents;
}
void readVectorToCharArray(const std::vector<unsigned char>& data, unsigned char* output, std::size_t startIndex)
{
	// Calculate the number of bytes to copy
	std::size_t bytesToCopy = (data.size() - startIndex < 256) ? (data.size() - startIndex) : 256;

	// Copy the data from vector to array
	for (std::size_t i = 0; i < bytesToCopy; ++i) {
		output[i] = data[startIndex + i];
	}
}
void writeCharArrayToFile(const char* fileName, const unsigned char* data, std::size_t dataSize)
{
	std::ofstream file(fileName, std::ios::binary); // Open the file in binary mode

	if (!file.is_open()) {
		printf("ERROR: File open error...\n");
		return;
	}


	file.write(reinterpret_cast<const char*>(data), dataSize);

	// Check if the write operation was successful
	if (!file.good()) {
		printf("ERROR: Could not write to file...\n");
	}


	file.close();
}