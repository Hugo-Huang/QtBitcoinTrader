// Copyright (C) 2013 July IGHOR.
// I want to create trading application that can be configured for any rule and strategy.
// If you want to help me please Donate: 1d6iMwjjNo8ZGYeJBZKXgcgVk9o7fXcjc
// For any questions please use contact form https://sourceforge.net/projects/bitcointrader/
// Or send e-mail directly to julyighor@gmail.com
//
// You may use, distribute and copy the Qt Bitcion Trader under the terms of
// GNU General Public License version 3

#include "julyhttp.h"
#include <openssl/hmac.h>
#include "main.h"
#include <QTimer>

JulyHttp::JulyHttp(const QString &hostN, const QByteArray &restLine, QObject *parent)
	: QObject(parent)
{
	connectionClose=false;
	bytesDone=0;
	contentLength=0;
	chunkedSize=-1;
	readingHeader=false;
	waitingReplay=false;
	isDisabled=false;
	outGoingPacketsCount=0;

	socket=new QSslSocket(this);
	setupSocket(socket);
	
	requestTimeOut.restart();
	hostName=hostN;
	httpHeader.append(" HTTP/1.1\r\n");
	httpHeader.append("User-Agent: Qt Bitcoin Trader v"+appVerStr+"\r\n");
	httpHeader.append("Host: "+hostName+"\r\n");
	httpHeader.append("Connection: keep-alive\r\n");
	apiDownState=false;
	apiDownCount=0;
	restKeyLine=restLine;

	QTimer *secondTimer=new QTimer(this);
	connect(secondTimer,SIGNAL(timeout()),this,SLOT(sendPendingData()));
	secondTimer->start(300);
}

JulyHttp::~JulyHttp()
{
	abortSocket();
}

void JulyHttp::setupSocket(QSslSocket *socket)
{
	socket->setSocketOption(QAbstractSocket::LowDelayOption,true);
	socket->setSocketOption(QAbstractSocket::KeepAliveOption,true);
	connect(socket,SIGNAL(readyRead()),SLOT(readSocket()));
	connect(socket,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(errorSlot(QAbstractSocket::SocketError)));
	connect(socket,SIGNAL(sslErrors(const QList<QSslError> &)),this,SLOT(sslErrorsSlot(const QList<QSslError> &)));
}

void JulyHttp::clearPendingData()
{
	for(int n=requestList.count()-1;n>=0;n--)takeRequestAt(n);
	reConnect();
}

void JulyHttp::reConnect(bool mastAbort)
{
	if(isDisabled)return;
	reconnectSocket(socket,mastAbort);
	retryRequest();
}

void JulyHttp::abortSocket()
{
	if(socket==0)return;
	socket->blockSignals(true);
	socket->abort();
	socket->blockSignals(false);
}

void JulyHttp::reconnectSocket(QSslSocket *socket, bool mastAbort)
{
	if(isDisabled)return;
	if(socket==0)return;
	if(mastAbort)abortSocket();
	if(socket->state()==QAbstractSocket::UnconnectedState||socket->state()==QAbstractSocket::UnconnectedState)
		socket->connectToHostEncrypted(hostName, 443, QIODevice::ReadWrite);
}

void JulyHttp::setApiDown(bool httpError)
{
	if(httpError)apiDownCount++;else apiDownCount=0;

	bool currentApiDownState=apiDownCount>5;
	if(apiDownState!=currentApiDownState)
	{
		apiDownState=currentApiDownState;
		emit apiDown(apiDownState);
	}
}

void JulyHttp::readSocket()
{
	if(isDisabled)return;

	emit anyDataReceived();
	requestTimeOut.restart();

	if(!waitingReplay)
	{
		connectionClose=false;
		buffer.clear();
		waitingReplay=true;
		readingHeader=true;
		contentLength=0;
	}

	while(readingHeader)
	{
		bool endFound=false;
		QString currentLine;
		while(!endFound&&socket->canReadLine())
		{
			currentLine=socket->readLine().toLower();
			if(currentLine==QLatin1String("\r\n")||
			   currentLine==QLatin1String("\n")||
			   currentLine.isEmpty())endFound=true;
			else
			{
				if(currentLine.startsWith("set-cookie"))cookie=currentLine.toAscii();
				else
				if(currentLine.startsWith("transfer-encoding")&&
				   currentLine.endsWith("chunked\r\n"))chunkedSize=0;
				else
				if(currentLine.startsWith(QLatin1String("content-length")))
				{
					QStringList pairList=currentLine.split(":");
					if(pairList.count()==2)contentLength=pairList.last().trimmed().toUInt();
				}
				else
				if(currentLine.startsWith(QLatin1String("connection"))&&
					currentLine.endsWith(QLatin1String("close\r\n")))connectionClose=true;
			}
		}
		if(!endFound)
		{
			retryRequest();
			return;
		}
		readingHeader=false;
	}

	bool allDataReaded=false;

		qint64 readSize=socket->bytesAvailable();
		QByteArray *dataArray=0;
		if(chunkedSize!=-1)
		{
			while(true)
			{
				if(chunkedSize==0)
				{
					if(!socket->canReadLine())break;
					QString sizeString=socket->readLine();
					int tPos=sizeString.indexOf(QLatin1Char(';'));
					if(tPos!=-1)sizeString.truncate(tPos);
					bool ok;
					chunkedSize=sizeString.toInt(&ok,16);
					if(!ok)
					{
						if(isLogEnabled)logThread->writeLog("Invalid size");
						if(dataArray){delete dataArray;dataArray=0;}
						retryRequest();
						return;
					}
					if(chunkedSize==0)chunkedSize=-2;
				}

				while(chunkedSize==-2&&socket->canReadLine())
				{
					QString currentLine=socket->readLine();
					 if(currentLine==QLatin1String("\r\n")||
						currentLine==QLatin1String("\n"))chunkedSize=-1;
				}
				if(chunkedSize==-1)
				{
					allDataReaded=true;
					break;
				}

				readSize=socket->bytesAvailable();
				if(readSize==0)break;
				if(readSize==chunkedSize||readSize==chunkedSize+1)
				{
					readSize=chunkedSize-1;
					if(readSize==0)break;
				}

				qint64 bytesToRead=chunkedSize<0?readSize:qMin(readSize,chunkedSize);
				if(!dataArray)dataArray=new QByteArray;
				uint oldDataSize=dataArray->size();
				dataArray->resize(oldDataSize+bytesToRead);
				qint64 read=socket->read(dataArray->data()+oldDataSize,bytesToRead);
				dataArray->resize(oldDataSize+read);

				chunkedSize-=read;
				if(chunkedSize==0&&readSize-read>=2)
				{
					char twoBytes[2];
					socket->read(twoBytes,2);
					if(twoBytes[0]!='\r'||twoBytes[1]!='\n')
					{
						if(isLogEnabled)logThread->writeLog("Invalid HTTP chunked body");
						if(dataArray){delete dataArray;dataArray=0;}
						retryRequest();
						return;
					}
				}
			}
		} 
		else
			if(contentLength>0)
			{
			readSize=qMin(qint64(contentLength-bytesDone),readSize);
			if(readSize>0)
			{
				if(dataArray){delete dataArray;dataArray=0;}
				dataArray=new QByteArray;
				dataArray->resize(readSize);
				dataArray->resize(socket->read(dataArray->data(),readSize));
			}
			if(bytesDone+socket->bytesAvailable()+readSize==contentLength)allDataReaded=true;
			}
			else 
			if(readSize>0)
			{
			if(!dataArray)dataArray=new QByteArray(socket->readAll());
			}

		if(dataArray)
		{
			readSize=dataArray->size();
				buffer.append(*dataArray);
				if(dataArray){delete dataArray;dataArray=0;}
				if(contentLength>0)
					emit dataProgress((bytesDone+socket->bytesAvailable())/contentLength);
		}
		if(dataArray){delete dataArray;dataArray=0;}

	if(allDataReaded)
	{
		if(!buffer.isEmpty()&&requestList.count())
		{
			bool apiMaybeDown=buffer[0]=='<';
			setApiDown(apiMaybeDown);
			if(!apiMaybeDown)emit dataReceived(buffer,requestList.first().second);
		}
		waitingReplay=false;
		readingHeader=true;
		takeFirstRequest();
		clearRequest();
		if(connectionClose)reConnect(true);
		sendPendingData();
	}
}

bool JulyHttp::isReqTypePending(int val)
{
	return reqTypePending.value(val,0)>0;
}

void JulyHttp::retryRequest()
{
	if(isDisabled)return;
	if(requestRetryCount<=0)takeFirstRequest();
	else requestRetryCount--;
	sendPendingData();
}

void JulyHttp::clearRequest()
{
	requestRetryCount=0;
	buffer.clear();
	chunkedSize=-1;
	nextPacketMastBeSize=false;
	endOfPacket=false;
}

void JulyHttp::prepareData(int reqType, const QByteArray &method, QByteArray postData, const QByteArray &restSignLine)
{
	if(isDisabled)return;
	QByteArray *data=new QByteArray(method+httpHeader+cookie);
	if(!restSignLine.isEmpty())data->append(restKeyLine+restSignLine);
	if(!postData.isEmpty())
	{
		data->append("Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+QByteArray::number(postData.size())+"\r\n\r\n");
		data->append(postData);
	}
	else data->append("\r\n");

	QPair<QByteArray*,int> reqPair;
	reqPair.first=data;
	reqPair.second=reqType;
	preparedList<<reqPair;
	retryCountMap[data]=1;
	reqTypePending[reqType]=reqTypePending.value(reqType,0)+1;
}

void JulyHttp::prepareDataSend()
{
	if(isDisabled)return;
	if(preparedList.count()==0)return;

	for(int n=1;n<preparedList.count();n++)
	{
		preparedList.at(0).first->append(*(preparedList.at(n).first))+"\r\n\r\n";
		skipOnceMap[preparedList.at(n).first]=true;
	}
	for(int n=0;n<preparedList.count();n++)requestList<<preparedList.at(n);
	preparedList.clear();
}

void JulyHttp::prepareDataClear()
{
	if(isDisabled)return;
	for(int n=0;n<preparedList.count();n++)
	{
		QPair<QByteArray*,int> reqPair=preparedList.at(n);
		retryCountMap.remove(reqPair.first);
		reqTypePending[reqPair.second]=reqTypePending.value(reqPair.second,1)-1;
		if(reqPair.first)delete reqPair.first;
	}
	preparedList.clear();
}

void JulyHttp::sendData(int reqType, bool isVip, const QByteArray &method, int removeLowerReqTypes, QByteArray postData, const QByteArray &restSignLine)
{
	if(isDisabled)return;
	Q_UNUSED(removeLowerReqTypes);
	QByteArray *data=new QByteArray(method+httpHeader+cookie);
	if(!restSignLine.isEmpty())data->append(restKeyLine+restSignLine);
	if(!postData.isEmpty())
	{
		data->append("Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "+QByteArray::number(postData.size())+"\r\n\r\n");
		data->append(postData);
	}
	else data->append("\r\n");

	QPair<QByteArray*,int> reqPair;
	reqPair.first=data;
	reqPair.second=reqType;
	//if(false&&removeLowerReqTypes>100)
	//{
	//	for(int n=requestList.count()-1;n>=1;n--)
	//		if(requestList.at(n).second<removeLowerReqTypes)takeRequestAt(n);
	//}
	if(isVip)retryCountMap[data]=2;
	else retryCountMap[data]=0;
	requestList<<reqPair;

	reqTypePending[reqType]=reqTypePending.value(reqType,0)+1;
	sendPendingData();
}

void JulyHttp::takeRequestAt(int pos)
{
	if(requestList.count()<=pos)return;
	QPair<QByteArray*,int> reqPair=requestList.at(pos);
	reqTypePending[reqPair.second]=reqTypePending.value(reqPair.second,1)-1;
	retryCountMap.remove(reqPair.first);
	if(isLogEnabled)logThread->writeLog("Data taken: "+*reqPair.first);
	delete reqPair.first;
	reqPair.first=0;
	requestList.removeAt(pos);
	if(requestList.count()==0)
	{
		reqTypePending.clear();
		retryCountMap.clear();
	}
}

void JulyHttp::takeFirstRequest()
{
	if(requestList.count()==0)return;
	takeRequestAt(0);
}

void JulyHttp::errorSlot(QAbstractSocket::SocketError socketError)
{
	setApiDown(true);

	if(isLogEnabled)logThread->writeLog("SocketError: "+socket->errorString().toAscii());

	if(socketError==QAbstractSocket::ProxyAuthenticationRequiredError)
	{
		isDisabled=true;
		emit errorSignal(socket->errorString());
		abortSocket();
	}
	else reconnectSocket(socket,false);
}

bool JulyHttp::isSocketConnected(QSslSocket *socket)
{
	return socket->state()==QAbstractSocket::ConnectedState;
}

QSslSocket *JulyHttp::getStableSocket()
{
	if(isSocketConnected(socket))return socket;
	else reconnectSocket(socket,false);

	if(socket->state()!=QAbstractSocket::UnconnectedState)socket->waitForConnected(5000);
	if(socket->state()!=QAbstractSocket::ConnectedState)
	{
		reconnectSocket(socket,false);
		socket->waitForConnected(5000);
	}
	else reconnectSocket(socket,false);
	return socket;
}

void JulyHttp::sendPendingData()
{
	if(isDisabled)return;
	if(requestList.count()==0)return;

	QSslSocket *currentSocket=getStableSocket();
	if(requestList.count()==0)return;
	QByteArray *pendingRequest=pendingRequestMap.value(currentSocket,0);
	if(pendingRequest==requestList.first().first)
	{
		if(requestTimeOut.elapsed()<httpRequestTimeout)return;
		else
		{
			if(isLogEnabled)logThread->writeLog(QString("Request timeout: %0>%1").arg(requestTimeOut.elapsed()).arg(httpRequestTimeout).toAscii());
			reconnectSocket(socket,true);
			if(requestRetryCount>0){retryRequest();return;}
		}
	}
	else
	{
		pendingRequestMap[currentSocket]=requestList.first().first;
		pendingRequest=pendingRequestMap.value(currentSocket,0);
	}
	clearRequest();
	requestRetryCount=retryCountMap.value(pendingRequest,1);
	if(requestRetryCount<1||requestRetryCount>100)requestRetryCount=1;
	requestTimeOut.restart();
	if(isLogEnabled)logThread->writeLog("SND: "+*pendingRequest);

	if(pendingRequest)
	{
		if(skipOnceMap.value(pendingRequest,false)==true)skipOnceMap.remove(pendingRequest);
		else
		{
			if(currentSocket->bytesAvailable())
			{
				if(isLogEnabled)logThread->writeLog("Cleared previous data: "+currentSocket->readAll());
				else currentSocket->readAll();
			}
			waitingReplay=false;
			currentSocket->write(*pendingRequest);
			currentSocket->flush();
		}
	}
	else if(isLogEnabled)logThread->writeLog("PendingRequest pointer not exist");
}

void JulyHttp::sslErrorsSlot(const QList<QSslError> &val)
{
	emit sslErrorSignal(val);
}