#include<iostream>
#include <chrono>
#include<queue>
#include<map>
#include<vector>
#include<fstream>
#include<mutex>
#include<unordered_map>
#include<list>

#include <stdlib.h>
#include <Windows.h>
#include <stdio.h>
#include <Tchar.h>

using namespace std;
using namespace chrono;
using namespace chrono_literals;


#define TUNIT short
#define TOTALTIME 10
#define FLOOR_PEOPLE_FEED 2
#define TOTALFLOOR 3
#define TOTALCARTS 2
#define CART_CAPA 9
//#define ONEPERSON_MOVE_TIME 100ms
#define NO_TARFLOOR 99999
#define TIME_ONE_JUMP 3s
#define TIME_ONE_STOP 2s
#define TIME_ONE_PASS 1s
#define TIME_DUMP 1s
#define TIME_LOAD 1s
#define PAUSE_CART_ARRIVE 1s
#define PAUSE_FLOORMGR 2s
//#define CART_NUM 1

class floorMgr;
class RequestMgr;

short underRunning = 1;
mutex mtxUnderRunning;
condition_variable cvUnderRunning;
vector<floorMgr> fMgrs(TOTALFLOOR);

HANDLE wHnd;    // Handle to write to the console.
HANDLE rHnd;    // Handle to read from the console.

const int X = 185;
const int Y = 50;

const int XBackGround = TOTALFLOOR*6+1;
const int YBackGround = 1+ TOTALCARTS *3+2;
// Set up the character buffer:
CHAR_INFO floorIdxBuffer[XBackGround * YBackGround];


class linked_set : public map<int, list<int>::iterator> {
public:
	//keep original default constructor, read ops(size, find), iterators on read(begin, end) 
	//may forbid cp, mv facility later

	//should hide original insert, erase etc

	void myPush_back(int k) {//assume no duplicate
		if (this->find(k) == this->end()) {
			auto it = l_.insert(l_.end(), k);
			this->insert({ k,it });
		}
		else {
			//throw
		}
	}

	void myPop_front() {
		if (this->empty()) {
			//throw
		}
		int rlt = *(l_.begin());
		l_.pop_front();
		this->erase(rlt);
	}

	int myFront() {
		if (this->empty()) {
			//throw
		}
		int rlt = *(l_.begin());
		return rlt;
	}

	void myErase(int k) {
		if (this->find(k) != this->end()) {
			auto it = this->operator[](k);
			l_.erase(it);
			this->erase(k);
		}
		else {
			throw "no such element in linked_set!";
		}
	}
private:
	list<int> l_;
	//do not use original insert erase
};


//blocking linked set
class RequestMgr {
private:
	mutex mtx_;
	condition_variable cv;
	linked_set requests_;

	int p_sequatialpop(unique_lock<mutex>& ulock) {
		//unique_lock<mutex> ulock(mtx_);
		while (requests_.empty()) cv.wait(ulock);
		int request = requests_.myFront();
		requests_.myPop_front();
		return request;
	}

public:
	void addReuqest(int r) {//r is the floor of request, + represents up, - represents down 
		unique_lock<mutex> ulock(mtx_);
		requests_.myPush_back(r);
		cv.notify_one();
	}


	//only for one direction, ex, if currFloor >=0, only check if there is any requests over this floor

	//used some cart go up to the highest floor with all passengers go out and no up passengers at floor curr
	//it need to check and pick if there is any request at or over this floor
	//if not, return curr and pick down passengers at this floor if any
	//if there is down passengers, then blocking-pop
	int BlockPop(int signedFloor) {
		unique_lock<mutex> ulock(mtx_);
		if (requests_.empty()) {
			return p_sequatialpop(ulock);
		}

		int rlt;
		if (signedFloor >= 0) {//direction is up
			//auto it = lower_bound(requests_.begin(), requests_.end(), signedFloor);
			auto it = requests_.lower_bound(signedFloor);
			if (it != requests_.end()) {//it should be the lowest up-request floor that is >= curr
				rlt = it->first;
				requests_.myErase(rlt);
				return rlt;
			}
			else {
				it = requests_.begin();//it should be the highest floor among all down-request floors
				rlt = it->first;
				requests_.myErase(rlt);
				return rlt;
			}
		}
		else {//direction is down
			//auto it = lower_bound(requests_.begin(), requests_.end(), signedFloor);
			auto it = requests_.lower_bound(signedFloor);
			if (it != requests_.end()) {//it should be the highest down-request floor that is <= curr
										//or lowest floor among all up-request floors
				rlt = it->first;
				requests_.myErase(rlt);
				return rlt;
			}
			else {
				it = requests_.begin();//it should be the highest floor among all down requests
				rlt = it->first;
				requests_.myErase(rlt);
				return rlt;
			}

		}
	}

	//used for greedy picking
	bool getAndErase(int nextFloor) {//signed floor represents a request 
		unique_lock<mutex> ulock(mtx_);
		int rlt;
		auto it = requests_.find(nextFloor);
		if (it != requests_.end()) {
			rlt = it->first;
			requests_.myErase(rlt);
			return true;
		}
		return false;
	}
};
RequestMgr requestMgr;

struct WaitGroup {
	//TUNIT arriveTime_;
	short count_;
	//unsigned char 
	short tarFloor_;

	WaitGroup() :count_(0) {}
};

istream& operator>> (istream& is, WaitGroup& wg) {
	is >> wg.count_ >> wg.tarFloor_;
	return is;
}

void generateWaitGroupsForOneFloor(ofstream& ofs, int totalTime, int people,int currFloor, int totalFloor) {// 

	for (int i = 0; i < totalTime; i++) {
		if (rand() % totalTime < people) {
			ofs << 1<<' ';
			int tarFloor=currFloor;
			while (tarFloor == currFloor) {
				tarFloor = rand() % totalFloor;
			}
			ofs << tarFloor<<' ';
		}
		else {
			ofs << 0 <<' '<<0 <<' ';
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
			unique_lock<mutex> lock_floorData(mtx_);
			if (time_ < TOTALTIME) {
				if (timeserialWaitGroup_[time_].count_ == 0) {
					continue;
				}
				//access queue
				if (timeserialWaitGroup_[time_].tarFloor_ < thisFloor_) {
					downLine_.push(timeserialWaitGroup_[time_]);
					downWaitCount_ += timeserialWaitGroup_[time_].count_;
					if (downRequest_ == 0) {
						downRequest_ = 1;
						requestMgr.addReuqest(-thisFloor_);
					}
					UpdateThisFloor(*this, lock_floorData, true);
				}
				else {
					upLine_.push(timeserialWaitGroup_[time_]);
					upWaitCount_ += timeserialWaitGroup_[time_].count_;
					if (upRequest_ == 0) {
						upRequest_ = 1;
						requestMgr.addReuqest(thisFloor_);
					}
					UpdateThisFloor(*this, lock_floorData,false);
				}
			}

			if (time_ >= TOTALTIME && upWaitCount_ == 0 && downWaitCount_ == 0) {
				//cout
				break;
			}
			lock_floorData.unlock();
			//sleep
			this_thread::sleep_for(PAUSE_FLOORMGR);
		}
	}

	//or to assume that we hold the lock when enter this function
	void static UpdateThisFloor(floorMgr& fMgr, unique_lock<mutex>& lock_floorData, bool isDown) {

		short base = XBackGround * (1+TOTALCARTS*3+1);
		short offset = 1 + fMgr.thisFloor_ * 6;
		int& count = isDown ? fMgr.downWaitCount_ : fMgr.upWaitCount_;
		char& requestSign = isDown ? fMgr.downRequest_ : fMgr.upRequest_;

		if (!isDown) {
			offset += 3;
		}
		if (count >= 10) {
			floorIdxBuffer[base + offset].Char.AsciiChar = '0' + (count) / 10;
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
		COORD characterPos = { offset,1+TOTALCARTS*3 };
		SMALL_RECT writeArea = { offset,1 + TOTALCARTS * 3,offset + 1,1 + TOTALCARTS * 3+1 };

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
	Cart() :id_(count++),peopleTotal_(0), goToCountEachFloor_(TOTALFLOOR), direction_(1),newDir_(1), currFloor_(0), tarFloor_(NO_TARFLOOR), isStopping_(true), shouldPickNext_(false), shouldNextStop_(false)
	{

	}

	void operator()() {
		while (1) {
			unique_lock<mutex> ul(mtxUnderRunning);
			if (underRunning == -1) {
				break;
			}
			while (underRunning == 0) {
				cvUnderRunning.wait(ul);
			}
			ul.unlock();

			//dump first
			if (isStopping_) {//either at first or just by some outgoing passengers
				if (goToCountEachFloor_[currFloor_]) {
					peopleTotal_ -= goToCountEachFloor_[currFloor_];
					goToCountEachFloor_[currFloor_] = 0;
					UpdateCartNumPeople(*this);
					this_thread::sleep_for(TIME_DUMP);
				}

				//if no people inside and not to pick this floor, no task here, get one
				if (peopleTotal_ == 0 && !shouldPickNext_) {//this also means peopleTotal_==0
					int request = requestMgr.BlockPop(currFloor_);
					if (abs(request) > currFloor_) {
						newDir_ = 1;
					}
					else if (abs(request) < currFloor_) {
						newDir_ = -1;
					}
					else {
						newDir_ = request >= 0 ? 1 : -1;
						shouldPickNext_ = true;//should pick this floor
					}
					direction_ = newDir_;
					tarFloor_ = abs(request);
				}
				//a non-stopping cart will always have a task
			}

			//before leave, check this floor
			if (isStopping_) {//either at first or just by some outgoing passengers
				pickTheDirectionRequestAtThisFloor();
			}
			//at here, target floor should not be currFloor

			//check next floor
			//if next floor is tarFloor, shouldPick
			//shouldPickNext_ = false;
			//shouldNextStop_ = false;
			shouldPickNext_ = (currFloor_+direction_==tarFloor_);
			//if (currFloor_ + direction_ >= 0 && currFloor_ + direction_ < TOTALFLOOR) {

			//}
			//ASSERT(currFloor_ + direction_ >= 0);
			if (goToCountEachFloor_[currFloor_ + direction_] == 0&& !shouldPickNext_) {
				//if shouldPick isi false yet and no dump next floor, try if there is people to pick next floor
				shouldPickNext_ |= requestMgr.getAndErase((currFloor_ + direction_) * direction_);
			}

			shouldNextStop_ = shouldPickNext_ | (goToCountEachFloor_[currFloor_ + direction_]!=0);
						
			//sleep
			if (isStopping_ && shouldNextStop_) {
				this_thread::sleep_for(TIME_ONE_JUMP);
			}
			else if (!isStopping_&&!shouldNextStop_) {
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

	void static UpdateCartLocation(Cart& cart) {
		int currFloor = cart.currFloor_;
		short base = XBackGround * (2+cart.id_*3);
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
			characterPos = { psIdx,2+cart.id_*3 };
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
		short base = XBackGround * (2+cart.id_*3);
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



private:
	static short count;
	void pickTheDirectionRequestAtThisFloor() {//it is either ordered previously by this cart, 
										//or check currently when arriving at this floor
		//assume that it is not full
		if (shouldPickNext_||requestMgr.getAndErase(direction_*currFloor_)) {
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
				auto wt = q.front();
				int move = wt.count_;//>0
				if (peopleTotal_ + wt.count_ < CART_CAPA) {
					q.pop();

				}
				else {
					int move = CART_CAPA - peopleTotal_;
					full = true;
				}
				//change fm
				count -= move;

				//change cart
				peopleTotal_ += wt.count_;
				goToCountEachFloor_[wt.tarFloor_] += wt.count_;
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
				requestMgr.addReuqest(direction_ * currFloor_);
			}
			floorMgr::UpdateThisFloor(fMgrs[currFloor_], lFloor, direction_==-1);
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
	int tarFloor_;

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


int main() {
	//feedData();

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

	init();
	renderBackGround();

	thread fMgrsThreads[TOTALFLOOR];
	for (int f = 0; f < TOTALFLOOR; f++) {
		fMgrsThreads[f] = thread(&floorMgr::operator(), ref(fMgrs[f]));
	}

	thread cartsThreads[TOTALCARTS];
	for (int i = 0; i < 2; i++) {
		cartsThreads[i] = thread(&Cart::operator(), ref(carts[i]));
	}


	//join
	for (int f = 0; f < TOTALFLOOR; f++) {
		fMgrsThreads[f].join();
	}

	for (int i = 0; i < TOTALCARTS; i++) {
		cartsThreads[i].join();
	}


}