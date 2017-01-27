//!
//! \file TLMInterface.h
//!
//! Provides a definition for the TLMParameter class
//!
//!
//! \author   Iakov Nakhimovski
//!

#ifndef TLMPARAMETER_H
#define TLMPARAMETER_H


#include <queue>
#include <vector>
#include <string>
#include "TLMCommUtil.h"
#include "TLMClientComm.h"
#include "common.h"


//!
//! TLMParameter provides the client side functionality for a single TLM parameter
//!
class TLMParameter {

 public:

    //! TLMParameter constructor. Sends a registration message to the TLM manager
    //! and prepares the object for simulation.
    TLMParameter(TLMClientComm& theComm, std::string& aName, std::string& aDefaultValue);

    //! Get name of the interface
    const std::string& GetName() const { return Name; }

    //! Get interface ID of this interface
    int GetParameterID() const { return  ParameterID; }

    std::string GetValue() const { return Value; }

 protected:
    //! Name of this TLM parameter
    std::string Name;

    //! TLM ID of this interface as received from the TLM manager
    int ParameterID;

    std::string Value;

    //! The communication object of the client
    TLMClientComm& Comm;

    //! Message buffer used to transfer information between different methods
    TLMMessage Message;
};

#endif // TLMPARAMETER_H
