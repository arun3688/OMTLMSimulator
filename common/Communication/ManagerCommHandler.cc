#include "Communication/ManagerCommHandler.h"
#include "tostr.h"
#include <iostream>
#include <sstream>
#ifdef USE_THREADS
#include <pthread.h>
#endif
#include <cassert>
#include <algorithm>

#include <cstdlib>
#ifndef NO_RTIME
#include "timing.h"
#else
#include "SurrogateTimer.h"
#endif //NO_RTIME

#ifndef _MSC_VER
#include <unistd.h> 
#endif


using std::string;
using std::cerr;
using std::endl;
using std::multimap;

// Run method executes all the protocols in the right order:
// Startup, Check then Simulate
void ManagerCommHandler::Run(CommunicationMode CommMode_In) {
    CommMode = CommMode_In;

#ifdef USE_THREADS
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr,  PTHREAD_SCOPE_SYSTEM);
    pthread_t reader, writer;

#if 1
    pthread_t monitor;
    if(CommMode == CoSimulationMode) {
        pthread_create(&monitor, &attr, thread_MonitorThreadRun, (void*)this);
    }
#endif

    // start the reader & writer threads
    pthread_create(&reader, &attr, thread_ReaderThreadRun, (void*)this);

    pthread_create(&writer, &attr, thread_WriterThreadRun, (void*)this);

#if 1
    if(CommMode == CoSimulationMode) {
        pthread_join(monitor, NULL);
    }
#endif
    pthread_join(reader, NULL);
    pthread_join(writer, NULL);
#endif

    if(exceptionMsg.size() > 0) {
        throw(exceptionMsg);
    }

}

//! Thread exception handler.
void ManagerCommHandler::HandleThreadException(const std::string &msg) {
    exceptionLock.lock();

    exceptionMsg += msg + "\n";

    // Terminate message queue, this will unblock the writer thread if needed.
    MessageQueue.Terminate();

    // We close all sockets on exception.
    Comm.CloseAll();

    exceptionLock.unlock();
}

bool ManagerCommHandler::GotException(std::string &msg) {
    msg = exceptionMsg;
    return (msg.size() > 0);
}

// RunStartupProtocol implements startup protocol that
// enables client registration at the manager
void ManagerCommHandler::RunStartupProtocol() {
    // Number of components that are expected to register
    int numToRegister = TheModel.GetComponentsNum();
    // Number of components waiting for check model reply
    int numCheckModel = 0;

    // Server socket is used to accept connections
    int acceptSocket = Comm.CreateServerSocket();
    
    // Update the meta-model with the selected server port.
    TheModel.GetSimParams().SetPort(Comm.GetServerPort());
    
    // Start the external components forming "coupled simulation"
    TheModel.StartComponents();

    TLMErrorLog::Info("-----  Waiting for registration requests  ----- ");
    Comm.AddActiveSocket(acceptSocket);
    
    // Setup timer
    tTM_Info tInfo;
    TM_Init(&tInfo);
    TM_Start(&tInfo);


    while((numToRegister > 0) || (numCheckModel < TheModel.GetComponentsNum())) {
        Comm.SelectReadSocket();
        
        // Check for timeout.
        TM_Stop(&tInfo);
        if(tInfo.total.tv_sec > TheModel.GetSimParams().GetTimeout()) {
            Comm.CloseAll();
            TLMErrorLog::FatalError("Timeout - failed to start all components, give up! ("
                                    + TLMErrorLog::ToStdStr(int(tInfo.total.tv_sec))
                                    + " > " + TLMErrorLog::ToStdStr(TheModel.GetSimParams().GetTimeout())
                                    + ")");
            break;
        }
        // Restart is needed for correct time accumulation.
        TM_Start(&tInfo);
        
        TLMErrorLog::Info("Communicating with clients...");
        
        Comm.ClearActiveSockets();
        // Check all the components for interface registration messages
        for(int iSock =  TheModel.GetComponentsNum() - 1; iSock >= 0; --iSock) {
            TLMComponentProxy& comp =  TheModel.GetTLMComponentProxy(iSock);
            int hdl = comp.GetSocketHandle();
            // The component needs to be connected to a socket
            if(hdl < 0) continue;
            // The component needs to be is startup mode, not waiting for check mode
            if(comp.GetReadyToSim()) continue;

            // There is data waiting
            if(!Comm.HasData(hdl)) {
                Comm.AddActiveSocket(hdl); // expect more messages
                continue;
            }

            TLMMessage* message = MessageQueue.GetReadSlot();
            message->SocketHandle = hdl;
            TLMCommUtil::ReceiveMessage(*message);

            if(message->Header.MessageType ==  TLMMessageTypeConst::TLM_CHECK_MODEL) {
                // This component is done with registration. It's will wait for others
                TLMErrorLog::Info(string("Component ") + comp.GetName() + " is ready to simulation");;

                comp.SetReadyToSim();
                numCheckModel++;
                MessageQueue.ReleaseSlot(message);
            }
            else if(message->Header.MessageType == TLMMessageTypeConst::TLM_REG_PARAMETER) {
                TLMErrorLog::Info(string("Component ") + comp.GetName() + " registers parameter");

                Comm.AddActiveSocket(hdl);
                ProcessRegParameterMessage(iSock, *message);
                MessageQueue.PutWriteSlot(message);
            }
            else {
                TLMErrorLog::Info(string("Component ") + comp.GetName() + " registers interface");;

                Comm.AddActiveSocket(hdl); // expect more messages
                ProcessRegInterfaceMessage(iSock, *message);
                MessageQueue.PutWriteSlot(message);
            }
        }

        // Check if a new connection is waiting to be accepted.
        if((numToRegister > 0) && Comm.HasData(acceptSocket)) {
            int hdl = Comm.AcceptComponentConnections();
            // WARNING!!! This is potentially a problematic case
            // since I immediately try to receive a message from just accepted connection
            // and might block in 'read' while other clients wait.
            // Alternatively I could put the sockets that are not associated
            // with any component to a separate place and "select" on them once more.
            // Might be necessary to fix later.

            TLMMessage* message = MessageQueue.GetReadSlot();
            message->SocketHandle = hdl;
            if(!TLMCommUtil::ReceiveMessage(*message)) {
                MessageQueue.ReleaseSlot(message);
                TLMErrorLog::FatalError("Failed to get message, exiting");
                abort();
            }

            ProcessRegComponentMessage(*message);

            MessageQueue.PutWriteSlot(message);
            numToRegister --;
            if(numToRegister == 0)
                TLMErrorLog::Info("All expected components are registered");

            Comm.AddActiveSocket(hdl);
        }

        if(numToRegister)  // still more connections expected
            Comm.AddActiveSocket(acceptSocket);
        
    }
}

// ProcessRegComponentMessage processes the first message after "accept"
// It is expected to be a component registration message.
// The functions associates the socket handle with the component in the CompositeModel.
// It then prepares the reply by setting TLMInterfaceID to component ID.
void ManagerCommHandler::ProcessRegComponentMessage(TLMMessage& mess) {

    if(mess.Header.MessageType !=  TLMMessageTypeConst::TLM_REG_COMPONENT) {
        TLMErrorLog::FatalError("Component registration message expected");
    }

    string aName((const char*)(& mess.Data[0]), mess.Header.DataSize);

    int CompID = TheModel.GetTLMComponentID(aName);

    if(CompID < 0 || CompID >= TheModel.GetComponentsNum()) {
        TLMErrorLog::FatalError("Component registration for " + aName + " failed!");
        return;
    }

    TLMComponentProxy& comp = TheModel.GetTLMComponentProxy(CompID);

    comp.SetSocketHandle(mess.SocketHandle);

    mess.Header.DataSize = 0;

    mess.Header.TLMInterfaceID = CompID;
    
    TLMErrorLog::Info(string("Component ") + aName + " is connected");

}

// ProcessRegInterfaceMessage processes a TLMInterface registration message from a client.
// It finds the appropriate proxy, sets its status to "connected"
// and prepares a reply message with interface ID and connection parameters.
// Note, that it's ok to try to register an interface not defined
// in the model. It'll just mean that no information will be sent to/from it.
void ManagerCommHandler::ProcessRegInterfaceMessage(int compID, TLMMessage& mess) {
    if(mess.Header.MessageType != TLMMessageTypeConst::TLM_REG_INTERFACE) {
        //std::cerr << "wrong message is: " <<  mess.Header.MessageType << endl;
        //std::cerr << "wrong message is: " <<  int(mess.Header.MessageType) << endl;
        TLMErrorLog::FatalError("Interface registration message expected");
    }

    // First, find the interface in the meta model
    string aSpecification ((const char*)(& mess.Data[0]), mess.Header.DataSize);

    TLMErrorLog::Info("Manager received nameAndType: "+aSpecification);

    string aName, dimStr, causality, domain;
    if(aSpecification.find(':') == std::string::npos) {     //This is for backwards compatibility with BEAST.
        dimStr = "6";                                         //Remove this later when  BEAST supports dimensions,
        causality="bidirectional";                          //causality and domain.
        domain="mechanical";
    }
    bool readingName=true;
    bool readingDimensions=false;
    bool readingCausality=false;
    bool readingDomain=false;
    for(size_t i=0; i<aSpecification.size(); ++i) {
        if(aSpecification[i] == ':' && readingName) {
            readingName = false;
            readingDimensions = true;
        }
        else if(aSpecification[i] == ':' && readingDimensions) {
            readingDimensions = false;
            readingCausality = true;
        }
        else if(aSpecification[i] == ':' && readingCausality) {
            readingCausality = false;
            readingDomain = true;
        }
        else if(readingDomain) {
            domain += aSpecification[i];
        }
        else if(readingCausality) {
            causality += aSpecification[i];
        }
        else if(readingDimensions) {
            dimStr += aSpecification[i];
        }
        else {
            aName += aSpecification[i];
        }
    }

    int dimensions;
    if(dimStr == "3D") {        //Backwards compatibility
        dimensions = 6;
    }
    else {
        dimensions = std::atoi(dimStr.c_str());
    }
    if(causality == "") {
        causality = "bidirectional";
    }
    if(domain == "") {
        domain = "mechanical";
    }

    int IfcID = TheModel.GetTLMInterfaceID(compID, aName);

    mess.Header.TLMInterfaceID = IfcID;

    mess.Header.SourceIsBigEndianSystem = TLMMessageHeader::IsBigEndianSystem;
    mess.Header.DataSize = 0;

    if(IfcID < 0 && CommMode == InterfaceRequestMode) {
        // interface not found, create it
        //std::string type = "1D";                                //HARD-CODED /robbr
        TheModel.RegisterTLMInterfaceProxy(compID, aName, dimensions,
                                           causality, domain);
        IfcID = TheModel.GetTLMInterfaceID(compID, aName);
    }

    if(IfcID < 0) {
        // interface not found
        TLMErrorLog::Warning(string("Interface ") +
                             TheModel.GetTLMComponentProxy(compID).GetName() + '.'
                             + aName + " not defined in composite model. Ignored.");
        return;
    }

    if(CommMode == CoSimulationMode) {
        SetupInterfaceConnectionMessage(IfcID, aName, mess);
    }
    else if(CommMode == InterfaceRequestMode) {
        
        TLMErrorLog::Info(string("Register TLM interface ") +
                         TheModel.GetTLMComponentProxy(compID).GetName() + '.' + aName);
        
        std::stringstream ss;
        ss << "Assigning interface ID = " << IfcID;
        TLMErrorLog::Info(ss.str());
        mess.Header.TLMInterfaceID = IfcID;
        
        TLMInterfaceProxy& ifc = TheModel.GetTLMInterfaceProxy(IfcID);
        ifc.SetConnected();

        SetupInterfaceRequestMessage(mess);
    }
    else {
        TLMErrorLog::Warning("Wrong communication mode in ManagerCommHandler::ProcessRegInterfaceMessage(...)");
        return;
    }
}

void ManagerCommHandler::ProcessRegParameterMessage(int compID, TLMMessage &mess) {
    if(mess.Header.MessageType != TLMMessageTypeConst::TLM_REG_PARAMETER) {
        //std::cerr << "wrong message is: " <<  mess.Header.MessageType << endl;
        //std::cerr << "wrong message is: " <<  int(mess.Header.MessageType) << endl;
        TLMErrorLog::FatalError("Parameter registration message expected");
    }

    // First, find the interface in the meta model
    string aNameAndValue((const char*)(& mess.Data[0]), mess.Header.DataSize);

    TLMErrorLog::Info("Manager received nameAndValue: "+aNameAndValue);

    string aName, aValue;
    bool readingName=true;
    for(size_t i=0; i<aNameAndValue.size(); ++i) {
        if(aNameAndValue[i] == ':' && readingName) {
            readingName = false;
        }
        else if(readingName) {
            aName += aNameAndValue[i];
        }
        else {
            aValue += aNameAndValue[i];
        }
    }

    int ParID = TheModel.GetComponentParameterID(compID, aName);

    mess.Header.SourceIsBigEndianSystem = TLMMessageHeader::IsBigEndianSystem;
    mess.Header.DataSize = 0;

    if(ParID < 0 && CommMode == InterfaceRequestMode) {
        // interface not found, create it
        //std::string type = "1D";                                //HARD-CODED /robbr
        TheModel.RegisterComponentParameterProxy(compID, aName, aValue);
        ParID = TheModel.GetComponentParameterID(compID, aName);
    }

    if(ParID < 0) {
        // interface not found
        TLMErrorLog::Warning(string("Parameter ") +
                             TheModel.GetTLMComponentProxy(compID).GetName() + '.'
                             + aName + " not defined in composite model. Ignored.");
        return;
    }

    std::stringstream ss;
    ss << "Assigning parameter ID = " << ParID;
    TLMErrorLog::Info(ss.str());

    mess.Header.ComponentParameterID = ParID;

    char ValueBuf[100];
    sprintf(ValueBuf, "%.99s", TheModel.GetComponentParameterProxy(ParID).GetValue().c_str());
    mess.Header.DataSize = sizeof(ValueBuf);
    mess.Data.resize(sizeof(TLMConnectionParams));
    memcpy(& mess.Data[0], &ValueBuf, mess.Header.DataSize);
}

void ManagerCommHandler::SetupInterfaceConnectionMessage(int IfcID, std::string& aName, TLMMessage& mess) {
    // set the connected flag in the CompositeModel
    TLMInterfaceProxy& ifc = TheModel.GetTLMInterfaceProxy(IfcID);
    ifc.SetConnected();

    // Find the connection object if exists

    int connID = ifc.GetConnectionID();
    if(connID < 0) {
        // interface is not connected in the meta-model
        mess.Header.TLMInterfaceID = -1;
        return;
    }

    TLMErrorLog::Info(string("Interface ") + aName + " is connected");

    // Put connection parameters in the reply
    TLMConnection& conn = TheModel.GetTLMConnection(connID);

    TLMConnectionParams& param = conn.GetParams();

    // Apply component transformation for each interface.
    int CompId = ifc.GetComponentID();
    TheModel.GetTLMComponentProxy(CompId).GetInertialTranformation(param.cX_R_cG_cG, param.cX_A_cG);

    // Send initial interface position
    //if(ifc.GetDimensions() == 9) {
    TLMTimeData3D& td = ifc.getTime0Data3D();
    for(int i=0; i<3; i++) param.Nom_cI_R_cX_cX[i] = td.Position[i];
    for(int i=0; i<9; i++) param.Nom_cI_A_cX[i] = td.RotMatrix[i];
    //}
    //    else if(ifc.GetDimensions() == 3 && ifc.GetCausality() == "bidirectional") {
    //        TLMTimeData1D& td = ifc.getTime0Data1D();
    //        param.Nom_cI_R_cX_cX[0] = 0;        param.Nom_cI_R_cX_cX[1] = 0;    param.Nom_cI_R_cX_cX[2] = 0;
    //        param.Nom_cI_A_cX[0] = td.Position; param.Nom_cI_A_cX[1] = 0;       param.Nom_cI_A_cX[2] = 0;
    //        param.Nom_cI_A_cX[3] = 0;           param.Nom_cI_A_cX[4] = 1;       param.Nom_cI_A_cX[5] = 0;
    //        param.Nom_cI_A_cX[6] = 0;           param.Nom_cI_A_cX[7] = 0;       param.Nom_cI_A_cX[8] = 1;
    //    }
    //    else {
    //        TLMTimeData1D& td = ifc.getTime0DataSignal();
    //        param.Nom_cI_R_cX_cX[0] = 0;    param.Nom_cI_R_cX_cX[1] = 0;    param.Nom_cI_R_cX_cX[2] = 0;
    //        param.Nom_cI_A_cX[0] = 1;       param.Nom_cI_A_cX[1] = 0;       param.Nom_cI_A_cX[2] = 0;
    //        param.Nom_cI_A_cX[3] = 0;       param.Nom_cI_A_cX[4] = 1;       param.Nom_cI_A_cX[5] = 0;
    //        param.Nom_cI_A_cX[6] = 0;       param.Nom_cI_A_cX[7] = 0;       param.Nom_cI_A_cX[8] = 1;
    //    }

    mess.Header.DataSize = sizeof(TLMConnectionParams);

    mess.Data.resize(sizeof(TLMConnectionParams));

    memcpy(& mess.Data[0], &param, mess.Header.DataSize);

}

void ManagerCommHandler::SetupInterfaceRequestMessage(TLMMessage& mess) {
    TLMConnectionParams param;
    param.Delay = 0.1;
    param.mode = 1;

    mess.Header.DataSize = sizeof(TLMConnectionParams);
    mess.Data.resize(sizeof(TLMConnectionParams));
    memcpy(& mess.Data[0], &param, mess.Header.DataSize);
    
}


// ReaderThreadRun processes incomming messages and creates
// messages to be sent.
void ManagerCommHandler::ReaderThreadRun() {

    // Handle start-up
    RunStartupProtocol();

    // Check that startup completed correctly
    int StartupOK = TheModel.CheckProxyComm();
    
    // Send the status result to all components
    for(int iSock =  TheModel.GetComponentsNum() - 1; iSock >= 0; --iSock) {
        int hdl = TheModel.GetTLMComponentProxy(iSock).GetSocketHandle();
        TLMMessage* message = MessageQueue.GetReadSlot();
        message->SocketHandle = hdl;
        message->Header.MessageType = TLMMessageTypeConst::TLM_CHECK_MODEL;
        message->Header.DataSize = 0;
        message->Header.TLMInterfaceID = StartupOK;
        MessageQueue.PutWriteSlot(message);
    }

    if(!StartupOK) {
        MessageQueue.Terminate();
        return;
    }

    TLMErrorLog::Info("------------------  Starting time data exchange   ------------------");
    
    Comm.SwitchToRunningMode();
    runningMode = RunMode;

    int nClosedSock = 0;
    std::vector<int> closedSockets;
    while(nClosedSock < TheModel.GetComponentsNum() || DisconnectedMonitors.size() < MonitorSockets.size()) {
        Comm.SelectReadSocket(); // wait for a change

        for(int iSock =  TheModel.GetComponentsNum() - 1; iSock >= 0; --iSock) {
            TLMComponentProxy& comp = TheModel.GetTLMComponentProxy(iSock);
            int hdl = comp.GetSocketHandle();

            if((std::find(closedSockets.begin(), closedSockets.end(), iSock) == closedSockets.end())
               && (hdl != 0) && Comm.HasData(hdl)) { // there is data to be received on the socket

                TLMMessage* message = MessageQueue.GetReadSlot();
                message->SocketHandle = hdl;
                if(TLMCommUtil::ReceiveMessage(*message)) {
                    if(message->Header.MessageType == TLMMessageTypeConst::TLM_CLOSE_REQUEST) {
                        MessageQueue.ReleaseSlot(message);
                        TLMErrorLog::Info("Received close permission request from "+comp.GetName());
                        closedSockets.push_back(iSock);
                        nClosedSock++;
                    }
                    else if(CommMode == CoSimulationMode) {
                        MarshalMessage(*message);

                        // Forward message for monitoring.
                        ForwardToMonitor(*message);

                        // Place in send buffer
                        MessageQueue.PutWriteSlot(message);
                    }
                    else {
                        // CommMode == InterfaceRequestMode
                        UnpackAndStoreTimeData(*message);
                        MessageQueue.ReleaseSlot(message);
                    }
                }
                else {
                    //Socket was closed without permission
                    nClosedSock++;
                    MessageQueue.ReleaseSlot(message);
                }
            }
        }
    }

    TLMErrorLog::Info("Simulation complete.");

    for(int iSock : closedSockets) {
      TLMMessage message;
      TLMComponentProxy& comp = TheModel.GetTLMComponentProxy(iSock);
      int hdl = comp.GetSocketHandle();
      message.SocketHandle = hdl;
      TLMErrorLog::Info("Sending close permission to "+comp.GetName());
      message.Header.MessageType = TLMMessageTypeConst::TLM_CLOSE_PERMISSION;
      TLMCommUtil::SendMessage(message);
      Comm.DropActiveSocket(hdl);
      comp.SetSocketHandle(-1);
      TLMErrorLog::Info(string("Connection to component ") + comp.GetName() + " is closed");
    }

    //Send close permission to all monitors
    for(int iSock : DisconnectedMonitors) {
        TLMErrorLog::Info("Sending close permission to monitor");
        TLMMessage message;
        message.SocketHandle = iSock;
        message.Header.MessageType = TLMMessageTypeConst::TLM_CLOSE_PERMISSION;
        TLMCommUtil::SendMessage(message);
    }
    MonitorsDisconnected = true;

    TLMErrorLog::Info("All sockets are closed.");
    runningMode = ShutdownMode;
    MessageQueue.Terminate();

    Comm.CloseAll();
}

void ManagerCommHandler::WriterThreadRun() {

    TLMMessage* tlm_mess = 0;
    TLMErrorLog::Info(string("TLM manager is ready to send messages"));

    while((tlm_mess = MessageQueue.GetWriteSlot()) != NULL) {
        TLMCommUtil::SendMessage(*tlm_mess);
        //TLMMessage &mm = *tlm_mess;
        //TLMCommUtil::SendMessage(mm);
        MessageQueue.ReleaseSlot(tlm_mess);
    }
    
}


void ManagerCommHandler::MarshalMessage(TLMMessage& message) {

  TLMInterfaceProxy& src = TheModel.GetTLMInterfaceProxy(message.Header.TLMInterfaceID);

  if(message.Header.MessageType !=   TLMMessageTypeConst::TLM_TIME_DATA) {
        TLMErrorLog::Info("Interface ID: "+TLMErrorLog::ToStdStr(message.Header.TLMInterfaceID));
        TLMErrorLog::FatalError("Unexpected message received from "+
                                TheModel.GetTLMComponentProxy(src.GetComponentID()).GetName()+
                                "."+src.GetName()+
                                +": " + ToStr(message.Header.MessageType));
    }

    // forward the time data
    int destID = src.GetLinkedID();

    if(destID < 0) {
        TLMErrorLog::Warning("Received time data for an unconnected interface. Ignored.");
        message.SocketHandle = -1;
        message.Header.TLMInterfaceID = -1;
    }
    else {
        TLMInterfaceProxy& dest = TheModel.GetTLMInterfaceProxy(destID);
        TLMComponentProxy& destComp = TheModel.GetTLMComponentProxy(dest.GetComponentID());
        message.SocketHandle = destComp.GetSocketHandle();
        message.Header.TLMInterfaceID = destID;

        if(TLMErrorLog::GetLogLevel() >= TLMLogLevel::Info) {
            TLMErrorLog::Info(string("Forwarding from " +
                                    TheModel.GetTLMComponentProxy(src.GetComponentID()).GetName() + '.'+
                                    src.GetName()
                                    + " to " + destComp.GetName() + '.' + dest.GetName()));
        }
    }
}

void ManagerCommHandler::UnpackAndStoreTimeData(TLMMessage& message) {
    if(message.Header.MessageType !=   TLMMessageTypeConst::TLM_TIME_DATA) {
        std::stringstream ss;
        ss << "Message type = " << int(message.Header.MessageType);
        TLMErrorLog::Info(ss.str());
        TLMErrorLog::FatalError("Unexpected message received in ManagerCommHandler::UnpackAndStoreTimeData(...)");
    }

    TLMInterfaceProxy& ip = TheModel.GetTLMInterfaceProxy(message.Header.TLMInterfaceID);

    if(ip.GetDimensions() == 6 && ip.GetCausality() == "bidirectional") {
        // since mess.Data is continious we can just convert the pointer
        TLMTimeData3D* Next = (TLMTimeData3D*)(&message.Data[0]);

        // check if we have byte order missmatch in the message and perform
        // swapping if necessary
        bool switch_byte_order =
                (TLMMessageHeader::IsBigEndianSystem != message.Header.SourceIsBigEndianSystem);
        if(switch_byte_order)
            TLMCommUtil::ByteSwap(Next, sizeof(double),  message.Header.DataSize/sizeof(double));

        // forward the time data
        TLMTimeData3D& data = ip.getTime0Data3D();

        TLMErrorLog::Info("Unpack and store 3D time data for " + ip.GetName());
        data = *Next;
    }
    else if(ip.GetDimensions() == 1 && ip.GetCausality() == "bidirectional") {
        // since mess.Data is continious we can just convert the pointer
        TLMTimeData1D* Next = (TLMTimeData1D*)(&message.Data[0]);

        // check if we have byte order missmatch in the message and perform
        // swapping if necessary
        bool switch_byte_order =
                (TLMMessageHeader::IsBigEndianSystem != message.Header.SourceIsBigEndianSystem);
        if(switch_byte_order)
            TLMCommUtil::ByteSwap(Next, sizeof(double),  message.Header.DataSize/sizeof(double));

        // forward the time data
        TLMTimeData3D& data = ip.getTime0Data3D();

        TLMErrorLog::Info("Unpack and store 1D time data for " + ip.GetName());

        data.Position[0] = Next->Position; data.Position[1] = 0;   data.Position[2] = 0;

        data.RotMatrix[0] = 1;  data.RotMatrix[1] = 0;  data.RotMatrix[2] = 0;
        data.RotMatrix[3] = 0;  data.RotMatrix[4] = 1;  data.RotMatrix[5] = 0;
        data.RotMatrix[6] = 0;  data.RotMatrix[7] = 0;  data.RotMatrix[8] = 1;

        data.Velocity[0] = Next->Velocity; data.Velocity[1] = 0;   data.Velocity[2] = 0;
        data.Velocity[3] = 0;               data.Velocity[4] = 0;   data.Velocity[5] = 0;
    }
    else {
        // since mess.Data is continious we can just convert the pointer
        TLMTimeDataSignal* Next = (TLMTimeDataSignal*)(&message.Data[0]);

        // check if we have byte order missmatch in the message and perform
        // swapping if necessary
        bool switch_byte_order =
                (TLMMessageHeader::IsBigEndianSystem != message.Header.SourceIsBigEndianSystem);
        if(switch_byte_order)
            TLMCommUtil::ByteSwap(Next, sizeof(double),  message.Header.DataSize/sizeof(double));

        // forward the time data
        TLMTimeData3D& data = ip.getTime0Data3D();

        TLMErrorLog::Info("Unpack and store signal time data for " + ip.GetName());

        data.Position[0] = 1;   data.Position[1] = 0;   data.Position[2] = 0;

        data.RotMatrix[0] = 1;  data.RotMatrix[1] = 0;  data.RotMatrix[2] = 0;
        data.RotMatrix[3] = 0;  data.RotMatrix[4] = 1;  data.RotMatrix[5] = 0;
        data.RotMatrix[6] = 0;  data.RotMatrix[7] = 0;  data.RotMatrix[8] = 1;

        data.Velocity[0] = 0;   data.Velocity[1] = 0;   data.Velocity[2] = 0;
        data.Velocity[3] = 0;   data.Velocity[4] = 0;   data.Velocity[5] = 0;
    }
}


int ManagerCommHandler::ProcessInterfaceMonitoringMessage(TLMMessage& message) {
    if(message.Header.MessageType != TLMMessageTypeConst::TLM_REG_INTERFACE) {
        TLMErrorLog::FatalError("Interface monitoring registration message expected");
    }
    
    // First, find the interface in the meta model
    string aNameAndType ((const char*)(& message.Data[0]), message.Header.DataSize);
    string aName, type;
    bool readingType=false;
    for(size_t i=0; i<aNameAndType.size(); ++i) {
        if(aNameAndType[i] == ':') {
            readingType = true;
        }
        if(readingType) {
            type += aNameAndType[i];
        }
        else {
            aName += aNameAndType[i];
        }
    }

    TLMErrorLog::Info("Request for monitoring " + aName);

    // Here the full name, i.e., component.interface, is requered
    int IfcID = TheModel.GetTLMInterfaceID(aName);

    message.Header.TLMInterfaceID = IfcID;
    message.Header.SourceIsBigEndianSystem = TLMMessageHeader::IsBigEndianSystem;
    message.Header.DataSize = 0;
    
    if(IfcID < 0) {
        TLMErrorLog::Warning("In monitoring, interface " + aName + " is not connected.");
        return -1;
    }
    
    // Wait until interface registration is completet.
    TLMInterfaceProxy& ifc = TheModel.GetTLMInterfaceProxy(IfcID);
    while(!ifc.GetConnected()) {
#ifndef _MSC_VER
        usleep(10000); // micro seconds
#else
        Sleep(10); // milli seconds
#endif
    }



    string::size_type DotPos = aName.find('.');  // Component name is the part before '.'
    string IfcName = aName.substr(DotPos+1);
    
    SetupInterfaceConnectionMessage(IfcID, IfcName, message);

    return IfcID;
}

void ManagerCommHandler::ForwardToMonitor(TLMMessage& message) {
    if(MonitorsDisconnected)
        return;

    monitorMapLock.lock();

    // We forward to the sender!
    TLMInterfaceProxy& ifc = TheModel.GetTLMInterfaceProxy(message.Header.TLMInterfaceID);
    int TLMInterfaceID = ifc.GetLinkedID();
    
    if(monitorInterfaceMap.count(TLMInterfaceID) > 0) {

        if(message.Header.MessageType != TLMMessageTypeConst::TLM_TIME_DATA) {
            TLMErrorLog::FatalError("Unexpected message received in forward to monitor");
        }

        // Forward to all connected monitoring ports
        multimap<int,int>::iterator pos;
        for(pos = monitorInterfaceMap.lower_bound(TLMInterfaceID);
             pos != monitorInterfaceMap.upper_bound(TLMInterfaceID);
             pos++) {
            
            if(TLMErrorLog::GetLogLevel() >= TLMLogLevel::Info) {
                TLMErrorLog::Info("Forwarding to monitor, interface " + TLMErrorLog::ToStdStr(TLMInterfaceID)
                                  + " on socket " + TLMErrorLog::ToStdStr(pos->second));
            }
            
            int hdl = pos->second;
            
            TLMMessage* newMessage = MessageQueue.GetReadSlot();

            newMessage->SocketHandle = hdl;
            memcpy(&newMessage->Header, &message.Header, sizeof(TLMMessageHeader));
            newMessage->Header.TLMInterfaceID = TLMInterfaceID;

            newMessage->Header.DataSize = message.Header.DataSize;
            if(newMessage->Data.size() < newMessage->Header.DataSize) {
                newMessage->Data.resize(newMessage->Header.DataSize);
            }
            
            memcpy(&newMessage->Data[0], &message.Data[0], newMessage->Header.DataSize);

            MessageQueue.PutWriteSlot(newMessage);
        }
    }
    else {
        if(TLMErrorLog::GetLogLevel() >= TLMLogLevel::Info) {
            TLMErrorLog::Info("Nothing to forward for monitor interface " + TLMErrorLog::ToStdStr(TLMInterfaceID));
        }
    }
    monitorMapLock.unlock();
}


void ManagerCommHandler::MonitorThreadRun() {
    TLMErrorLog::Info("In monitoring");
    
    if(TheModel.GetSimParams().GetMonitorPort() <= 0) {
        TLMErrorLog::Info("Monitoring disabled!");
        return;
    }
    
    TLMErrorLog::Info("Initialize monitoring port");

    // Create a connection for max. 10 clients.
    TLMManagerComm monComm(10, TheModel.GetSimParams().GetMonitorPort());

    // Server socket is used to accept connections
    int acceptSocket = monComm.CreateServerSocket();
    if(acceptSocket == -1) {
        TLMErrorLog::FatalError("Failed to initialize monitoring socket");
        abort();
        //return;
    }

    if(TheModel.GetSimParams().GetMonitorPort() != monComm.GetServerPort()) {
        TLMErrorLog::Warning("Used monitoring port : " + TLMErrorLog::ToStdStr(monComm.GetServerPort()));
    }

    // Update the meta-model with the selected server port.
    TheModel.GetSimParams().SetMonitorPort(monComm.GetServerPort());

    // Never switch to running mode but use active sockets instead.
    monComm.AddActiveSocket(acceptSocket);

    TLMErrorLog::Info("Wait for monitoring connections...");

    std::multimap<int,int> localIntMap;

    //assert(runningMode == RunMode);
    while(runningMode != ShutdownMode && !MonitorsDisconnected) {
        int hdl = -1;

        monComm.SelectReadSocket();
        
        // Just check if we are in shutdown mode
        if(runningMode == ShutdownMode) break;

        if(monComm.HasData(acceptSocket)) {
            TLMErrorLog::Info("Got new monitoring connection");
            hdl = monComm.AcceptComponentConnections();
            if(hdl < 0) {
                TLMErrorLog::FatalError("Failed to accept socket.");
                abort();
            }
            monComm.AddActiveSocket(hdl);
            MonitorConnected = true;
            MonitorSockets.push_back(hdl);
        }
        else {
            for(std::vector<int>::iterator it=MonitorSockets.begin(); it != MonitorSockets.end(); it++) {
                if(monComm.HasData(*it)) {
                    TLMErrorLog::Info("Accepted data on monitoring connection");

                    hdl = *it;
                    break;
                }
            }
        }

        // Just check if we are in shutdown mode
        if(runningMode == ShutdownMode) break;

        if(hdl >= 0) {
            TLMMessage* message = MessageQueue.GetReadSlot();
            message->SocketHandle = hdl;
            
            if(!TLMCommUtil::ReceiveMessage(*message)) {
                TLMErrorLog::Warning("Failed to get message from monitor, disconected?");
                //abort();
                monComm.DropActiveSocket(hdl);
                MessageQueue.ReleaseSlot(message);
                continue;
            }
            
            if(message->Header.MessageType ==  TLMMessageTypeConst::TLM_CHECK_MODEL) {
                TLMErrorLog::Warning("Received unexpected check-model on monitoring interface, try to answer...");

                message->Header.TLMInterfaceID = 1;
                message->Header.DataSize = 0;

                MessageQueue.PutWriteSlot(message);
            }
            else if(message->Header.MessageType == TLMMessageTypeConst::TLM_CLOSE_REQUEST) {
                TLMErrorLog::Info("Received close permission from monitor.");
                monitorMapLock.lock();
                DisconnectedMonitors.push_back(message->SocketHandle);
                monitorMapLock.unlock();
                MessageQueue.ReleaseSlot(message);
            }
            else {
                int IfcID = ProcessInterfaceMonitoringMessage(*message);
                MessageQueue.PutWriteSlot(message);

                if(IfcID >= 0) {

                    TLMErrorLog::Info("Register monitor handle for interface " + ToStr(IfcID));
#if 0
                    //std::cout << "hdl=" << hdl << ", ifID=" << IfcID << std::endl;
                    localIntMap.insert(std::make_pair(hdl, IfcID));
                    //std::cout << "hdl count=" << localIntMap.count(hdl) << ", if count=" << TheModel.GetInterfacesNum() << std::endl;

                    // NOTE, here we register interfaces first if all interfaces are monitored.
                    if(localIntMap.count(hdl) == TheModel.GetInterfacesNum()) {
                        monitorMapLock.lock();
                        std::multimap<int,int>::iterator it;
                        for(it = localIntMap.lower_bound(hdl);
                             it != localIntMap.upper_bound(hdl);
                             it++) {
                            monitorInterfaceMap.insert(std::make_pair(it->second, hdl));
                        }
                        monitorMapLock.unlock();
                    }
                    else {
                    }
#else
                    monitorMapLock.lock();
                    monitorInterfaceMap.insert(std::make_pair(IfcID, hdl));
                    monitorMapLock.unlock();
#endif

                }
            }
            //MessageQueue.PutWriteSlot(message);
        }
        else {
            // wait some time.
#ifndef _MSC_VER
            usleep(10000);
#else
            Sleep(10);
#endif
        }

    }

    // Close all sockets
    monComm.CloseAll();
}
