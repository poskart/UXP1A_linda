#include "Request.h"
#include <iostream>
/**
 * 	@brief	Copy constructor
 */
Request::Request(const Request & r)
{
	this->procId = r.procId;
	this->reqType = r.reqType;
	this->timeout = r.timeout;
	this->tuple = new Tuple(*(r.tuple));
}

/**
 * 	@ Constructor
 */
Request::Request(Tuple * t, Request::ReqType rType, std::time_t timeout)
{
	this->procId = getpid();
	this->reqType = rType;
	this->timeout = timeout;
	this->tuple = t;
}

/**
 * 	@brief 	Gets data from the request and pushes it into ostream.
 *
 * 	@param	os is ostream object which data is pushed to
 * 	@param	req is Request object which provides request data
 */
std::ostream& operator<<(std::ostream &os, const Request& req) {
	os << req.procId << std::endl;
	os << (int)(req.reqType) << std::endl;
	os << req.timeout << std::endl;
	os << req.tuple->elems.size() << std::endl;
	for (unsigned i = 0; i < req.tuple->elems.size(); ++i) {
		os << req.tuple->elems[i].isString << std::endl;
		os << req.tuple->elems[i].pattern << std::endl;
	}
	return os;
}

/**
 *	@brief	Gets request from istream and pushes data into given Request object.
 *
 *	@param is is ifstream object wich passes Request data
 *	@param req is empty Request object to store request data
 */
std::istream& operator>>(std::istream &is, Request& req) {
	is >> req.procId;
	is >> req.reqType;
	is >> req.timeout;

//	std::cout<<"Operator >> (getting from ifstream)..."<<std::endl;
	Tuple *tuple = new Tuple();
	unsigned elemsCount;
	is >> elemsCount;
	for (unsigned i = 0; i < elemsCount; ++i) {
		bool isString;
		std::string tmp;
		is >> isString;
		is >> tmp;
		Elem el = Elem(isString, tmp);
		tuple->elems.push_back(el);
//		std::cout<<"tuple->elems pushed back..., isString: "<<isString << ", tmp: "<< tmp <<std::endl;
	}
	if(req.tuple != nullptr)
		delete req.tuple;
	req.tuple = tuple;
	return is;
}

/**
 * 	@brief	Sets new Tuple object to the Request
 *
 * 	@param	tuple is Tuple object which is assigned to this request.
 */
void Request::setTuple(Tuple * _tuple){
	if(this->tuple != nullptr)
		delete tuple;
	tuple = _tuple;
}
