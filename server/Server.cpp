#include "Server.h"
#include "ServerController.h"



static QString getIdentifier(QWebSocket *peer) {
    return QStringLiteral("%1:%2").arg(peer->peerAddress().toString(),
                                       QString::number(peer->peerPort()));
}


Server::Server(QObject *parent) : QObject(parent) {
    thread()->setObjectName("Main Thread");

    m_pWebSocketServer = new QWebSocketServer(QStringLiteral("Polidox Server"),
                                              QWebSocketServer::NonSecureMode, this);

    this->dbOperations = new DatabaseManager();

    if (m_pWebSocketServer->listen(QHostAddress::Any, PORT_NUMBER))
    {
        QTextStream(stdout) << "PoliDox server listening on port " << PORT_NUMBER << '\n';
        connect(m_pWebSocketServer, &QWebSocketServer::newConnection, this, &Server::onNewConnection);
    } else {
        qDebug() << "Couldn't bind to port " << PORT_NUMBER;
        qDebug() << m_pWebSocketServer->errorString();
    }
}


Account* Server::getAccount(QWebSocket *socketOfAccont){
    return this->socket2account[socketOfAccont];
}


//- To handle requests by not logged account.
//- The only valid actions can be "registerUser","loginReq"
//- Note that "genericRequestString" is a String type but interally
//  it's formatted in JSON
void Server::handleNotLoggedRequests(const QString& genericRequestString){
    QWebSocket *signalSender = qobject_cast<QWebSocket *>(QObject::sender());
    QJsonObject requestObjJSON;
    QJsonDocument requestDocJSON;

    requestDocJSON = QJsonDocument::fromJson(genericRequestString.toUtf8());
    requestObjJSON = requestDocJSON.object();

    // No switch case for strings in C++ :((
    QString header = requestObjJSON["action"].toString();
    if (header == "loginReq") {
        QString name = requestObjJSON["name"].toString();
        QString password = requestObjJSON["password"].toString();

        bool loginSuccess = false;
        Account *loggedAccount = nullptr;
        QList<QString> nameDocuments;

        //check if account is already logged. If true, login is rejected
        bool alreadyLogged = false;
        for(Account* account : this->socket2account.values()){
            if(account != nullptr && account->getName() == name){
                alreadyLogged = true;
            }
        }

        if(alreadyLogged){
            QByteArray sendMsgToClient = ServerMessageFactory::createLoginReply(loginSuccess, QString("log"), loggedAccount, nameDocuments);
            signalSender->sendTextMessage(sendMsgToClient);
            return;
        }

        QByteArray imageToReturn;
        int result = -1;
        try {
            result = this->dbOperations->checkPassword(name, password,  imageToReturn);
        } catch (mongocxx::query_exception) {
            QCoreApplication::removePostedEvents(nullptr);
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        };

        if(result < 0){
            QByteArray sendMsgToClient = ServerMessageFactory::createLoginReply(loginSuccess, QString("auth"), loggedAccount, nameDocuments);
            signalSender->sendTextMessage(sendMsgToClient);
            return;
        }

        loginSuccess = true;

        loggedAccount = new Account(result, name, imageToReturn);
        this->socket2account[signalSender] = loggedAccount;

        try{
            nameDocuments = this->dbOperations->getAllDocuments(loggedAccount->getSiteId());
        } catch (mongocxx::query_exception) {
            QCoreApplication::removePostedEvents(nullptr);
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        };

        disconnect(signalSender, &QWebSocket::textMessageReceived, this, &Server::handleNotLoggedRequests);
        connect(signalSender, &QWebSocket::textMessageReceived, this, &Server::handleLoggedRequests);

        QByteArray sendMsgToClient = ServerMessageFactory::createLoginReply(loginSuccess, QString(""), loggedAccount, nameDocuments);
        signalSender->sendTextMessage(sendMsgToClient);
    }
    else if (header == "registerUser"){
        QString name = requestObjJSON["name"].toString();
        QString password = requestObjJSON["password"].toString();

        if(requestObjJSON["image"].toString() == ""){
            // if the user does not provide an image, reject registration
            QByteArray sendMsgToClient = ServerMessageFactory::createRegistrationUserReply(false, -1);
            signalSender->sendTextMessage(sendMsgToClient);
            return;
        }

        // N.B. l'immagine viene salvata nel DB in base64
        QByteArray image = requestObjJSON["image"].toString().toLatin1();

        bool registrationSuccess = false;
        int siteId = -1;
        try{
            siteId = this->dbOperations->registerUser(name, password, image);
        } catch (mongocxx::query_exception) {
            QCoreApplication::removePostedEvents(nullptr);
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        };

        if(siteId >= 0)
            registrationSuccess = true;

        QByteArray sendMsgToClient = ServerMessageFactory::createRegistrationUserReply(registrationSuccess, siteId);
        signalSender->sendTextMessage(sendMsgToClient);
    }
    else {
        qWarning() << "Unknown message received: " << requestObjJSON["action"].toString();
    }

}


ServerController* Server::initializeServerController(QString& nameDocument, QString& uri, QList<Char>& orderedInserts){
    ServerController *fileServContr = new ServerController(nameDocument, uri, this);
    fileServContr->createCrdt(orderedInserts);
    return fileServContr;
}


//- To handle requests by logged account
//- The only valid actions can be "openFileReq","createFileReq"
void Server::handleLoggedRequests(const QString& genericRequestString){
    QWebSocket *signalSender = qobject_cast<QWebSocket *>(QObject::sender());

    QJsonObject requestObjJSON;
    QJsonDocument requestDocJSON;
    requestDocJSON = QJsonDocument::fromJson(genericRequestString.toUtf8());
    requestObjJSON = requestDocJSON.object();

    ServerController *fileServContr = nullptr;
    QString nameDocument = requestObjJSON["nameDocument"].toString();
    bool isFromUri = false;

    QString header = requestObjJSON["action"].toString();
    if (header == "openFileReq") {
        QString uri = requestObjJSON["uri"].toString();     //will be empty if it is not a uri request

        if (nameDocument == "") {
            //is a uri request
            try {
                nameDocument = this->dbOperations->getDocument(uri);
            } catch (mongocxx::query_exception) {
                QCoreApplication::removePostedEvents(nullptr);
                QCoreApplication::exit(EXIT_FAILURE);
                return;
            };

            if(nameDocument == ""){
                //the user has inserted an invalid uri
                QByteArray sendMsgToClient = ServerMessageFactory::createOpenFileReply(false, QString("uri"));
                signalSender->sendTextMessage(sendMsgToClient);
                return;
            } else {
                Account* loggedAccount = this->socket2account[signalSender];
                try {
                    this->dbOperations->insertNewPermission(nameDocument, loggedAccount->getSiteId());
                } catch (mongocxx::query_exception) {
                    QCoreApplication::removePostedEvents(nullptr);
                    QCoreApplication::exit(EXIT_FAILURE);
                    return;
                };

                isFromUri = true;
            }
        }

        if( !(this->file2serverController.contains(nameDocument)) ){
            QList<Char> orderedInserts;
            try{
                orderedInserts = this->dbOperations->getAllInserts(nameDocument);
            } catch (mongocxx::query_exception) {
                QCoreApplication::removePostedEvents(nullptr);
                QCoreApplication::exit(EXIT_FAILURE);
                return;
            };

            if(!isFromUri){
                try{
                    uri = dbOperations->getUri(nameDocument);
                } catch (mongocxx::query_exception) {
                    QCoreApplication::removePostedEvents(nullptr);
                    QCoreApplication::exit(EXIT_FAILURE);
                    return;
                };
            }

            fileServContr = this->initializeServerController(nameDocument, uri, orderedInserts);

            this->file2serverController[nameDocument] = fileServContr;
        } else {
            fileServContr = this->file2serverController[nameDocument];
        }

        fileServContr->addClient(signalSender);
        disconnect(signalSender, &QWebSocket::textMessageReceived, this, &Server::handleLoggedRequests);
        disconnect(signalSender, &QWebSocket::disconnected, this, &Server::disconnectAccount);
    }
    else if (header == "createFileReq"){
        //create and open the file
        //when a user creates a file he has, by default, the permission to open it

        if(nameDocument == ""){
            //the user can not insert a name that is an empty string
            QByteArray sendMsgToClient = ServerMessageFactory::createOpenFileReply(false, QString("name"));
            signalSender->sendTextMessage(sendMsgToClient);
            return;
        }

        Account* loggedAccount = this->socket2account[signalSender];
        QString uriOfDocument = this->generateUri(loggedAccount->getName(), nameDocument);

        bool result = false;
        try {
            result = this->dbOperations->insertNewDocument(nameDocument, uriOfDocument);   //false if there is already a document with that name
        } catch (mongocxx::operation_exception) {
            QCoreApplication::removePostedEvents(nullptr);
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        };

        if(!result){
            QByteArray sendMsgToClient = ServerMessageFactory::createOpenFileReply(false, QString("create"));
            signalSender->sendTextMessage(sendMsgToClient);
            return;
        }

        try{
            this->dbOperations->insertNewPermission(nameDocument, loggedAccount->getSiteId());
        } catch (mongocxx::query_exception) {
            QCoreApplication::removePostedEvents(nullptr);
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        };

        QList<Char> emptyList;
        fileServContr = this->initializeServerController(nameDocument, uriOfDocument, emptyList);
        this->file2serverController[nameDocument] = fileServContr;

        fileServContr->addClient(signalSender);
        disconnect(signalSender, &QWebSocket::textMessageReceived, this, &Server::handleLoggedRequests);
    }
    else {
        qWarning() << "Unknown message received: " << requestObjJSON["action"].toString();
    }
}


int Server::getDigits(std::string s) {
    long long int digits = 0;
    int runningLength = 0;
    int prevLength = 0;
    for ( auto it = s.rbegin(); it != s.rend(); ++it ) {
        runningLength += prevLength;
        //if digits is a 7 digit number then appending a 3 digit number would overflow an int
        digits += (long long int)*it * pow(10, runningLength);
        //we have to work out the length of the current digit
        //so we know how much we need to shift by next time
        int dLength = 0;
        for ( int d = *it; d > 0; dLength++, d /= 10 );
        prevLength = dLength;
        if ( digits >= 100000000 ) break;
    }
    return digits % 100000000;
}


QString Server::generateUri(QString nameAccount, QString& nameDocument){
    std::string uri("file://");
    int accountHashed = this->getDigits(nameAccount.toUtf8().constData());
    int documentHashed = this->getDigits(nameDocument.toUtf8().constData());

    uri = uri + std::to_string(accountHashed) + "/" + std::to_string(documentHashed) + "/" + nameDocument.toUtf8().constData();
    return QString::fromStdString(uri);
}


// if I arrive on this slot, is from a socket that in the
// moment he quits he does not have a open file
void Server::disconnectAccount(){
    QWebSocket *signalSender = qobject_cast<QWebSocket *>(QObject::sender());
    Account *accountToDisconnect = this->socket2account[signalSender];

    if(accountToDisconnect != nullptr){
        this->socket2account.remove(signalSender);
        delete (accountToDisconnect);
    }

    disconnect(signalSender, &QWebSocket::disconnected, this, &Server::disconnectAccount);
    delete (signalSender);
}


Server::~Server() {
    for(QWebSocket* elem : this->socket2account.keys()){
        Account* accToDelete = this->socket2account[elem];
        delete (accToDelete);

        disconnect(elem, &QWebSocket::disconnected, this, &Server::disconnectAccount);
        delete (elem);
    }

    for(ServerController* elem : this->file2serverController)
        delete (elem);

    delete (this->dbOperations);
    delete (this->m_pWebSocketServer);
}


void Server::onNewConnection() {
    QWebSocket *newSocket = m_pWebSocketServer->nextPendingConnection();
    QTextStream(stdout) << getIdentifier(newSocket) << " connected!\n";

    //until the account will not be logged in, the value is nullptr
    this->socket2account[newSocket] = nullptr;

    connect(newSocket, &QWebSocket::textMessageReceived, this, &Server::handleNotLoggedRequests);
    connect(newSocket, &QWebSocket::disconnected, this, &Server::disconnectAccount);
}


void Server::removeSocket2AccountPair(QWebSocket *socketOfAccount){
    this->socket2account.remove(socketOfAccount);
}


void Server::removeFile2ServcontrPair(QString filename){
   this->file2serverController.remove(filename);
}


DatabaseManager* Server::getDb(){
    return this->dbOperations;
}








