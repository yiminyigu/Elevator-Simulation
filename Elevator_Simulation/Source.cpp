#include<iostream>
#include <chrono>
#include<queue>
#include<map>
#include<vector>
#include<fstream>
#include<mutex>
#include<set>
#include<list>
#include<unordered_set>

//window.h facilities for rendering
#include <stdlib.h>
#include <Windows.h>
#include <stdio.h>
#include <Tchar.h>
using namespace std;
using namespace chrono;
using namespace chrono_literals;


#define TOTALTIME 10	//total number quantum for entire people-feed
#define FLOOR_PEOPLE_FEED 3		//ratio of people to feed for this amount of quantum(for stat)

#define TOTALFLOOR 30
#define TOTALCARTS 3
#define CART_CAPA 9		//how many people a cart can hold?

#define TIME_ONE_JUMP 3s	//time from one floor in stopping mode to next floor in stopping mode 
#define TIME_ONE_STOP 2s	//time from one floor in moving mode to next floor in stopping mode(vice versa)
#define TIME_ONE_PASS 1s	//time from one floor in moving mode to next floor in moving mode
#define TIME_DUMP 1s		//time to unload people
#define TIME_LOAD 1s		//time to load people
#define PAUSE_CART_ARRIVE 1s	//time for cart to stop steadily
#define PAUSE_FLOORMGR 30s		//time gap for floor manager to put in the people feed in line
 
//for internal signal use
#define NO_REQ 99999	
#define CART_DONE 0

//to indicate the number of finishing floorMgr thread
//used for end the block poll of cart and terminate the program
mutex mtxNumFloorMgrDone;
int numFloorMgrDone = 0;

//a handle to controll the pause, terminate or running of the program, I did not finish that yet
short underRunning = 1;
mutex mtxUnderRunning;
condition_variable cvUnderRunning;

class floorMgr;
class RequestMgr;

vector<floorMgr> fMgrs(TOTALFLOOR);		//global variable to record floor mgrs state

//rendering facilities
HANDLE wHnd;    // Handle to write to the console.
HANDLE rHnd;    // Handle to read from the console.
const int X = 185;
const int Y = 50;
const int XBackGround = TOTALFLOOR * 6 + 1;
const int YBackGround = 1 + TOTALCARTS * 3 + 2;
// Blackground buffer
CHAR_INFO floorIdxBuffer[XBackGround * YBackGround];

//file to record err and final statistics data
ofstream errf("err.txt");


class RequestMgr {
	//because of the order here, we pass the floor as 1 based floor into and from RequestMgr
//while use 0 based floor outside

private:
	mutex mtx_;
	condition_variable cv;
	set<int> requests_;
	unordered_set<int> intendedPick_;

public:
	void addReuqest(int r) {//r is the floor of request, + represents up, - represents down 
		unique_lock<mutex> ulock(mtx_);
		requests_.insert(r);
		cv.notify_one();
	}

	

	//used for pre-emptive picking
	bool getAndErase(int nextFloor) {//signed floor represents a request 
		unique_lock<mutex> ulock(mtx_);
		return getAndEraseHelper(nextFloor);
	}


	int upDateExternalReq(int floor, char direction, bool isStopping, int ExReq) {
		unique_lock<mutex> ulock(mtx_);
		
		{
			unique_lock<mutex> ulock(mtxNumFloorMgrDone);
			if (numFloorMgrDone == TOTALFLOOR && requests_.empty()&&intendedPick_.empty()) {
				return CART_DONE;
			}
		}

		if (intendedPick_.find(ExReq) != intendedPick_.end()) {
			//if reachable within one move
			if (isStopping && abs(floor - abs(ExReq)) <= 1 || !isStopping && floor + direction == abs(ExReq)) {
				intendedPick_.erase(ExReq);
				requests_.insert(ExReq);
				set<int>::iterator it;
				if (isStopping) {
					ExReq = nonBlockPop(floor * direction);
				}
				else {
					ExReq = nonBlockPop((floor + direction) * direction);
				}
			}//otherwise keep the original one
		}
		else {
			if (isStopping) {
				ExReq = BlockPop(direction * floor,ulock);
			}
			else {
				ExReq = nonBlockPop((floor + direction) * direction);
			}
		}
		//remove if pickable within one move
		if (isStopping) {
			if (abs(floor - abs(ExReq)) <= 1) {
				getAndEraseHelper(ExReq);
			}
		}
		else {
			if (floor + direction == abs(ExReq)) {
				getAndEraseHelper(ExReq);
			}
		}
		return ExReq;
	}

private:

	//assume that hold lock
	bool getAndEraseHelper(int nextFloor) {
		if (nextFloor == NO_REQ) {
			return false;
		}
		if (intendedPick_.find(nextFloor) != intendedPick_.end()) {
			intendedPick_.erase(nextFloor);
			return true;
		}
		int rlt;
		auto it = requests_.find(nextFloor);
		if (it != requests_.end()) {
			rlt = *it;
			requests_.erase(it);
			return true;
		}
		return false;
	}

	//assume that hold lock
	int BlockPop(int signedFloor, unique_lock<mutex>& ulock) {
		//unique_lock<mutex> ulock(mtx_);
		while (requests_.empty()) cv.wait(ulock);

		return nonBlockPop(signedFloor);
	}

	//assume that hold lock
	int nonBlockPop(int signedFloor) {
		int rlt;
		if (requests_.empty()) {
			return NO_REQ;
		}
		if (signedFloor >= 0) {//direction is up
			auto it = requests_.lower_bound(signedFloor);
			if (it != requests_.end()) {//it should be the lowest up-request floor that is >= curr
				rlt = *it;
				requests_.erase(it);
				intendedPick_.insert(rlt);
				return rlt;
			}
			else {
				it = requests_.begin();//it should be the highest floor among all down-request floors
				rlt = *it;
				requests_.erase(it);
				intendedPick_.insert(rlt);
				return rlt;
			}
		}
		else {//direction is down
			auto it = requests_.lower_bound(signedFloor);
			if (it != requests_.end()) {//it should be the highest down-request floor that is <= curr
										//or lowest floor among all up-request floors
				rlt = *it;
				requests_.erase(it);
				intendedPick_.insert(rlt);
				return rlt;
			}
			else {
				it = requests_.begin();//it should be the highest floor among all down requests
				rlt = *it;
				requests_.erase(it);
				intendedPick_.insert(rlt);
				return rlt;
			}

		}
	}
};
RequestMgr requestMgr;

struct WaitGroup {
	//TUNIT arriveTime_;
	short count_;
	//unsigned char 
	short tarFloor_;
	system_clock::time_point initWaitPoint_;
	WaitGroup() :count_(0) {}
};

istream& operator>> (istream& is, WaitGroup& wg) {
	is >> wg.count_ >> wg.tarFloor_;
	return is;
}

void generateWaitGroupsForOneFloor(ofstream& ofs, int totalTime, int people, int currFloor, int totalFloor) {// 

	for (int i = 0; i < totalTime; i++) {
		if (rand() % totalTime < people) {
			ofs << 1 << ' ';
			int tarFloor = currFloor;
			while (tarFloor == currFloor) {
				tarFloor = rand() % totalFloor;
			}
			ofs << tarFloor << ' ';
		}
		else {
			ofs << 0 << ' ' << 0 << ' ';
		}
	}
	ofs << '\n';
}

void feedData() {
	ofstream ofs("dataFeed.txt");
	for (int f = 0; f < TOTALFLOOR; f++) {
		generateWaitGroupsForOneFloor(ofs, TOTALTIME, FLOOR_PEOPLE_FEED, f, TOTALFLOOR);
	}
}


//act as waitpeople feeder of this floor
class floorMgr {
public:
	mutex mtx_;

	//used for FIFO fairness
	queue<WaitGroup> upLine_;//would also be accessed by carts when they are coming to decide whether to stop
	queue<WaitGroup> downLine_;

	//used for carts stop decision
	char upRequest_;//0: noRequest, 1 newRequest, 2 requestPickbysomecart
	char downRequest_;

	//used for rendering
	int upWaitCount_;
	int downWaitCount_;

	vector<WaitGroup> timeserialWaitGroup_;

	floorMgr() :time_(-1), thisFloor_(count++), upRequest_(0), downRequest_(0), upWaitCount_(0), downWaitCount_(0)
	{

	}

	void operator()() {
		try {
			while (1) {
				unique_lock<mutex> lock_underRunning(mtxUnderRunning);
				if (underRunning == -1) {
					break;
				}
				while (underRunning == 0) {
					cvUnderRunning.wait(lock_underRunning);
				}
				lock_underRunning.unlock();
				time_++;
				int nextTime = time_;
				while (nextTime < TOTALTIME && timeserialWaitGroup_[nextTime].count_ == 0) {
					nextTime++;
				}
				this_thread::sleep_for(PAUSE_FLOORMGR*(nextTime-time_));
				if (nextTime >= TOTALTIME) {
					unique_lock<mutex> lock_NumFloorMgrDone(mtxNumFloorMgrDone);
					numFloorMgrDone++;
					break;
				}
				time_ = nextTime;
				{
					unique_lock<mutex> lock_floorData(mtx_);
					//access queue
					if (timeserialWaitGroup_[time_].tarFloor_ < thisFloor_) {
						downLine_.push(timeserialWaitGroup_[time_]);
						downLine_.back().initWaitPoint_ = system_clock::now();
						downWaitCount_ += timeserialWaitGroup_[time_].count_;
						if (downRequest_ == 0) {
							downRequest_ = 1;
							requestMgr.addReuqest(-(thisFloor_ + 1));
						}
						UpdateThisFloor(*this, lock_floorData, true);
					}
					else {
						upLine_.push(timeserialWaitGroup_[time_]);
						upLine_.back().initWaitPoint_ = system_clock::now();
						upWaitCount_ += timeserialWaitGroup_[time_].count_;
						if (upRequest_ == 0) {
							upRequest_ = 1;
							requestMgr.addReuqest(thisFloor_ + 1);
						}
						UpdateThisFloor(*this, lock_floorData, false);
					}

				}
				this_thread::sleep_for(PAUSE_FLOORMGR);

			}


		}
		catch (std::exception & ex) {
			errf << " Failed!\n";
			errf << "\n    " << ex.what() << "\n";
			errf << "\n  exception caught at line " << __LINE__ << endl;
		}

	}

	//or to assume that we hold the lock when enter this function
	void static UpdateThisFloor(floorMgr& fMgr, unique_lock<mutex>& lock_floorData, bool isDown) {

		short base = XBackGround * (1 + TOTALCARTS * 3 + 1);
		short offset = 1 + fMgr.thisFloor_ * 6;
		int& count = isDown ? fMgr.downWaitCount_ : fMgr.upWaitCount_;
		char& requestSign = isDown ? fMgr.downRequest_ : fMgr.upRequest_;

		if (!isDown) {
			offset += 3;
		}
		if (count >= 10) {
			floorIdxBuffer[base + offset].Char.AsciiChar = '0' + (count) / 10;
		}
		else {
			floorIdxBuffer[base + offset].Char.AsciiChar = ' ';
		}
		floorIdxBuffer[base + offset + 1].Char.AsciiChar = '0' + (count) % 10;

		if (requestSign) {
			floorIdxBuffer[base - XBackGround + offset].Attributes =
				FOREGROUND_RED;
			floorIdxBuffer[base - XBackGround + offset + 1].Attributes =
				FOREGROUND_RED;
		}
		else {
			floorIdxBuffer[base - XBackGround + offset].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
			floorIdxBuffer[base - XBackGround + offset + 1].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		}

		COORD charBufSize = { XBackGround,YBackGround };
		COORD characterPos = { offset,1 + TOTALCARTS * 3 };
		SMALL_RECT writeArea = { offset,1 + TOTALCARTS * 3,offset + 1,1 + TOTALCARTS * 3 + 1 };

		// Write the characters:
		WriteConsoleOutputA(wHnd, floorIdxBuffer, charBufSize, characterPos, &writeArea);
	}

private:
	static short count;
	int time_;
	const short thisFloor_;
};
short floorMgr::count = 0;


class Cart {
public:
	Cart() :id_(count++), peopleTotal_(0), goToCountEachFloor_(TOTALFLOOR), direction_(1), newDir_(1), currFloor_(0), isStopping_(true), shouldPickNext_(false), shouldNextStop_(false), ExternalReq_(NO_REQ)
	{

	}

	void operator()() {
		try {
			while (1) {
				unique_lock<mutex> ul(mtxUnderRunning);
				if (underRunning == -1) {
					break;
				}
				while (underRunning == 0) {
					cvUnderRunning.wait(ul);
				}
				ul.unlock();

				if (isStopping_) {
					//dump
					if (goToCountEachFloor_[currFloor_]) {
						peopleTotal_ -= goToCountEachFloor_[currFloor_];
						goToCountEachFloor_[currFloor_] = 0;
						UpdateCartNumPeople(*this);
						this_thread::sleep_for(TIME_DUMP);
					}
					//load
					if (peopleTotal_ < CART_CAPA) {
						if (abs(ExternalReq_) == currFloor_) {
							direction_ = ExternalReq_ >= 0 ? 1 : -1;
						}

						pickTheDirectionRequestAtThisFloor();
						if (ExternalReq_ == direction_ * currFloor_) {
							ExternalReq_ = NO_REQ;
						}
					}
				}

				if (peopleTotal_ == 0) {
					ExternalReq_ = requestMgr.upDateExternalReq(currFloor_+1, direction_, isStopping_, ExternalReq_>=0?ExternalReq_+1:ExternalReq_-1);
					if (ExternalReq_ == CART_DONE) {
						break;
					}
					ExternalReq_ > 0 ? (ExternalReq_--) : (ExternalReq_++);
					if (isStopping_) {
						if (currFloor_ == abs(ExternalReq_)) {
							shouldPickNext_ = true;
							continue;
						}
						else {//change direction only when cart empty 
							direction_ = abs(ExternalReq_) > currFloor_ ? 1 : -1;
							if (abs(currFloor_ - abs(ExternalReq_)) <= 1) {
								shouldPickNext_ = true;
								isStopping_ = true;
								this_thread::sleep_for(TIME_ONE_JUMP);
								currFloor_ += direction_;
								UpdateCartLocation(*this);
								this_thread::sleep_for(PAUSE_CART_ARRIVE);
								continue;
							}
							//else normal floor checking
						}
					}
					else {

						if (currFloor_ + direction_ == abs(ExternalReq_)) {
							shouldPickNext_ = true;
							isStopping_ = true;
							this_thread::sleep_for(TIME_ONE_STOP);
							currFloor_ += direction_;
							UpdateCartLocation(*this);
							this_thread::sleep_for(PAUSE_CART_ARRIVE);
							continue;
						}
						else {
							if (direction_ == 1 && abs(ExternalReq_) <= currFloor_ || direction_ == -1 && abs(ExternalReq_ >= currFloor_)) {
								//shut down the direction first, then to reverse direction
								shouldPickNext_ = false;
								isStopping_ = true;
								this_thread::sleep_for(TIME_ONE_STOP);
								currFloor_ += direction_;
								direction_ = -direction_;
								UpdateCartLocation(*this);
								this_thread::sleep_for(PAUSE_CART_ARRIVE);
								continue;
							}
						}//else normal floor checking
					}
				}

				//check next floor
				//shouldPickNext_ = (currFloor_+direction_==tarFloor_);

				shouldPickNext_ = false;			//reset shouldPickNext_
				if (goToCountEachFloor_[currFloor_ + direction_] == 0 && peopleTotal_<CART_CAPA) {
					//if no dump next floor, try if there is people to pick next floor
					shouldPickNext_ |= requestMgr.getAndErase((currFloor_+1 + direction_) * direction_);
				}

				shouldNextStop_ = shouldPickNext_ | (goToCountEachFloor_[currFloor_ + direction_] != 0);

				//sleep
				if (isStopping_ && shouldNextStop_) {
					this_thread::sleep_for(TIME_ONE_JUMP);
				}
				else if (!isStopping_ && !shouldNextStop_) {
					this_thread::sleep_for(TIME_ONE_PASS);
				}
				else {
					this_thread::sleep_for(TIME_ONE_STOP);
				}
				//
				isStopping_ = shouldNextStop_;
				currFloor_ += direction_;
				UpdateCartLocation(*this);
				this_thread::sleep_for(PAUSE_CART_ARRIVE);
			}

		}
		catch (std::exception & ex) {
			errf << " Failed!\n";
			errf << "\n    " << ex.what() << "\n";
			errf << "\n  exception caught at line " << __LINE__ << endl;
		}

	}

	void static UpdateCartLocation(Cart& cart) {
		int currFloor = cart.currFloor_;
		short base = XBackGround * (2 + cart.id_ * 3);
		short offset = currFloor * 6;
		short endOff = offset + 6;
		short psIdx;
		if (cart.direction_ == 1) {
			psIdx = offset - 6;
		}
		else {
			psIdx = offset + 6;
		}

		//clear current
		for (int i = 0; i < 7; i++) {
			floorIdxBuffer[base + psIdx + i].Char.AsciiChar = ' ';
			FOREGROUND_RED;
			floorIdxBuffer[base + psIdx + i].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		}

		floorIdxBuffer[base + offset].Char.AsciiChar = '|';
		floorIdxBuffer[base + offset].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		int mid = offset + 3;
		floorIdxBuffer[base + mid].Char.AsciiChar = '0' + cart.peopleTotal_;
		floorIdxBuffer[base + mid].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[base + endOff].Char.AsciiChar = '|';
		floorIdxBuffer[base + endOff].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;


		COORD characterPos;
		SMALL_RECT writeArea;

		if (cart.direction_ == 1) {
			/*floorIdxBuffer[base + endOff].Attributes =
				FOREGROUND_GREEN;*/
			characterPos = { psIdx,2 + cart.id_ * 3 };
			writeArea = { psIdx,2 + cart.id_ * 3 ,psIdx + 12,2 + cart.id_ * 3 };
		}
		else {
			/*floorIdxBuffer[base + offset].Attributes =
				FOREGROUND_GREEN;*/
			characterPos = { offset,2 + cart.id_ * 3 };
			writeArea = { offset,2 + cart.id_ * 3 , offset + 12,2 + cart.id_ * 3 };
		}

		COORD charBufSize = { XBackGround,YBackGround };

		// Write the characters:
		WriteConsoleOutputA(wHnd, floorIdxBuffer, charBufSize, characterPos, &writeArea);
	}

	void UpdateCartNumPeople(Cart& cart) {
		int currFloor = cart.currFloor_;
		short base = XBackGround * (2 + cart.id_ * 3);
		short offset = currFloor * 6 + 3;
		floorIdxBuffer[base + offset].Char.AsciiChar = '0' + cart.peopleTotal_;
		FOREGROUND_RED;
		floorIdxBuffer[base + offset].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		COORD charBufSize = { XBackGround,YBackGround };
		COORD characterPos = { offset,2 + cart.id_ * 3 };
		SMALL_RECT writeArea = { offset,2 + cart.id_ * 3,offset,2 + cart.id_ * 3 };

		// Write the characters:
		WriteConsoleOutputA(wHnd, floorIdxBuffer, charBufSize, characterPos, &writeArea);

	}


	system_clock::duration totalWaitTime_ = system_clock::duration(0);
	int totalPickedPeople = 0;

private:
	static short count;
	void pickTheDirectionRequestAtThisFloor() {
		//if intend to pick when at previous floor, or intend to pick (if any) now
		if (shouldPickNext_ || requestMgr.getAndErase(direction_ * (currFloor_+1))) {
			//if we enter here, we should update the redenering
			unique_lock<mutex> lFloor(fMgrs[currFloor_].mtx_);
			floorMgr& fm = fMgrs[currFloor_];

			//change queue and count in fm
			char& requestSign = direction_ == 1 ? fm.upRequest_ : fm.downRequest_;
			int& count = direction_ == 1 ? fm.upWaitCount_ : fm.downWaitCount_;
			queue<WaitGroup>& q = direction_ == 1 ? fm.upLine_ : fm.downLine_;
			bool full = peopleTotal_ == CART_CAPA;
			while (!full && count > 0)
			{
				//not full for now
				auto& wt = q.front();
				short move = wt.count_;//>0
				if (peopleTotal_ + wt.count_ < CART_CAPA) {
					move= wt.count_;
					totalWaitTime_ += move * (system_clock::now() - wt.initWaitPoint_);
					totalPickedPeople += move;
					q.pop();

				}
				else {
					move = CART_CAPA - peopleTotal_;
					wt.count_ -= move;
					totalWaitTime_ += move * (system_clock::now() - wt.initWaitPoint_);
					totalPickedPeople += move;
					full = true;
				}
				//change fm
				count -= move;

				//change cart
				peopleTotal_ += move;
				goToCountEachFloor_[wt.tarFloor_] += move;
				//if (direction_ == 1) {
				//	tarFloor_ = max(wt.tarFloor_, tarFloor_);
				//}
				//else {
				//	tarFloor_ = min(wt.tarFloor_, tarFloor_);
				//}
			}

			//modify coresponding up/down request  
			if (count == 0) {
				requestSign = 0;
			}
			else {
				requestSign = 1;
				//help to re-add to requestMgr
				requestMgr.addReuqest(direction_ * (currFloor_+1));
			}
			floorMgr::UpdateThisFloor(fMgrs[currFloor_], lFloor, direction_ == -1);
			lFloor.unlock();//done with this floor's business
			UpdateCartNumPeople(*this);
			this_thread::sleep_for(TIME_LOAD);
		}
	}

	short id_;
	int peopleTotal_;
	char direction_;
	int currFloor_;//0 based

	vector<int> goToCountEachFloor_;
	char newDir_;
	//int tarFloor_;
	int ExternalReq_;

	bool shouldPickNext_;
	bool shouldNextStop_; //if we can hold more
	bool isStopping_;//final stop decision for next floor

};
short Cart::count = 0;


void init() {
	// Set up the handles for reading/writing:
	wHnd = GetStdHandle(STD_OUTPUT_HANDLE);
	rHnd = GetStdHandle(STD_INPUT_HANDLE);

	// Change the window title:
	//SetConsoleTitle(TEXT("Win32 Console Control Demo"));

	// Set up the required window size:
	SMALL_RECT windowSize = { 0, 0, X - 1, Y - 1 };

	// Change the console window size:
	SetConsoleWindowInfo(wHnd, TRUE, &windowSize);
	// Create a COORD to hold the buffer size:
	COORD bufferSize = { X, Y };

	// Change the internal buffer size:
	SetConsoleScreenBufferSize(wHnd, bufferSize);
	int totalLine = 1 + TOTALCARTS * 3 + 2;
	for (int i = 0; i < totalLine; i++) {
		printf("\n");
	}

}


void renderBackGround() {


	int sIdx = 0;
	int eIdx = 6;
	for (int i = 0; i < TOTALFLOOR; i++) {
		//deploy floorIdx
		int mid = (sIdx + eIdx) / 2;
		floorIdxBuffer[mid - 1].Char.AsciiChar = '[';
		floorIdxBuffer[mid - 1].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		if (i + 1 >= 10) {
			floorIdxBuffer[mid].Char.AsciiChar = '0' + (i + 1) / 10;
			floorIdxBuffer[mid].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
			mid++;
		}
		floorIdxBuffer[mid].Char.AsciiChar = '0' + (i + 1) % 10;
		floorIdxBuffer[mid].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[mid + 1].Char.AsciiChar = ']';
		floorIdxBuffer[mid + 1].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;


		//deploy intial carts
		if (i == 0) {
			int base = XBackGround * 2;
			for (int i = 0; i < TOTALCARTS; i++) {
				floorIdxBuffer[base + sIdx].Char.AsciiChar = '|';
				floorIdxBuffer[base + sIdx].Attributes =
					FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
				floorIdxBuffer[base + mid].Char.AsciiChar = '0';
				floorIdxBuffer[base + mid].Attributes =
					FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

				floorIdxBuffer[base + eIdx].Char.AsciiChar = '|';
				floorIdxBuffer[base + eIdx].Attributes =
					FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
				base += XBackGround * 3;
			}
		}
		int base = XBackGround * (1 + 3 * TOTALCARTS);
		//deploy waitpeople stat for each floor
		floorIdxBuffer[base + sIdx].Char.AsciiChar = '|';
		floorIdxBuffer[base + sIdx].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + sIdx].Char.AsciiChar = '|';
		floorIdxBuffer[base + XBackGround + sIdx].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[base + sIdx + 1].Char.AsciiChar = 'D';
		floorIdxBuffer[base + sIdx + 1].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + sIdx + 1].Char.AsciiChar = ' ';
		floorIdxBuffer[base + XBackGround + sIdx + 1].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[base + sIdx + 2].Char.AsciiChar = 'N';
		floorIdxBuffer[base + sIdx + 2].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + sIdx + 2].Char.AsciiChar = '0';
		floorIdxBuffer[base + XBackGround + sIdx + 2].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[base + sIdx + 4].Char.AsciiChar = 'U';
		floorIdxBuffer[base + sIdx + 4].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + sIdx + 4].Char.AsciiChar = ' ';
		floorIdxBuffer[base + XBackGround + sIdx + 4].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;

		floorIdxBuffer[base + sIdx + 5].Char.AsciiChar = 'P';
		floorIdxBuffer[base + sIdx + 5].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + sIdx + 5].Char.AsciiChar = '0';
		floorIdxBuffer[base + XBackGround + sIdx + 5].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;


		floorIdxBuffer[base + eIdx].Char.AsciiChar = '|';
		floorIdxBuffer[base + eIdx].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		floorIdxBuffer[base + XBackGround + eIdx].Char.AsciiChar = '|';
		floorIdxBuffer[base + XBackGround + eIdx].Attributes =
			FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;


		sIdx = eIdx;
		eIdx += 6;
	}

	int baserow = 1;
	for (int i = 0; i < TOTALCARTS; i++) {
		for (int j = 0; j < XBackGround; j++) {
			floorIdxBuffer[XBackGround * baserow + j].Char.AsciiChar = '-';
			floorIdxBuffer[XBackGround * baserow + j].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
			floorIdxBuffer[XBackGround * (baserow + 2) + j].Char.AsciiChar = '-';
			floorIdxBuffer[XBackGround * (baserow + 2) + j].Attributes =
				FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
		}
		baserow += 3;
	}


	// Set up the positions:
	COORD charBufSize = { XBackGround,YBackGround };
	COORD characterPos = { 0,0 };
	SMALL_RECT writeArea = { 0,0,XBackGround - 1,YBackGround - 1 };

	// Write the characters:
	WriteConsoleOutputA(wHnd, floorIdxBuffer, charBufSize, characterPos, &writeArea);
}



//main entrance
//you can comment the feedData line and it will use the previous data in datafeed.txt
//or if you changed the macro above, you may want to re-generate the datafeed.txt
int main() {
	//feedData();


	//extract people-feed
	vector<vector<WaitGroup>> waitGroupsPerFloor(TOTALFLOOR, vector<WaitGroup>(TOTALTIME));
	ifstream ifs("dataFeed.txt");
	for (int r = 0; r < TOTALFLOOR; r++) {
		for (int c = 0; c < TOTALTIME; c++) {
			ifs >> waitGroupsPerFloor[r][c];
		}
	}

	for (int f = 0; f < TOTALFLOOR; f++) {
		fMgrs[f].timeserialWaitGroup_ = waitGroupsPerFloor[f];
	}

	vector<Cart> carts(TOTALCARTS);

	//render
	init();
	renderBackGround();

	system_clock::time_point initTP = system_clock::now();
	thread fMgrsThreads[TOTALFLOOR];
	for (int f = 0; f < TOTALFLOOR; f++) {
		fMgrsThreads[f] = thread(&floorMgr::operator(), ref(fMgrs[f]));
	}

	thread cartsThreads[TOTALCARTS];
	for (int i = 0; i < TOTALCARTS; i++) {
		cartsThreads[i] = thread(&Cart::operator(), ref(carts[i]));
	}

	//join
	for (int f = 0; f < TOTALFLOOR; f++) {
		fMgrsThreads[f].join();
	}

	for (int i = 0; i < TOTALCARTS; i++) {
		cartsThreads[i].join();
	}

	//for final statistics
	system_clock::duration totalTime = system_clock::now() - initTP;
	system_clock::duration totalWaitTime = system_clock::duration(0);
	int totalPeople = 0;
	for (int i = 0; i < TOTALCARTS; i++) {
		totalWaitTime += carts[i].totalWaitTime_;
		totalPeople += carts[i].totalPickedPeople;
	}

	errf << totalPeople << " " << duration_cast<seconds>(totalWaitTime).count()<< " "<< duration_cast<seconds>(totalTime).count() <<endl;
	errf.close();
	ifs.close();
}