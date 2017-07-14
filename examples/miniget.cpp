/*
 * Copyright information and license terms for this software can be
 * found in the file LICENSE that is included with the distribution
 */
// The simplest possible PVA get

#include <iostream>

#include "pv/clientFactory.h"
#include "pv/pvaTestClient.h"

int main(int argc, char *argv[])
{
    try {
        if(argc<=1) {
            std::cerr<<"Usage: "<<argv[0]<<" <pvname>\n";
            return 1;
        }

        epics::pvAccess::ClientFactory::start();

        pvac::ClientProvider provider("pva");

        pvac::ClientChannel channel(provider.connect(argv[1]));

        std::cout<<channel.name()<<" : "<<channel.get()<<"\n";

    }catch(std::exception& e){
        std::cerr<<"Error: "<<e.what()<<"\n";
        return 1;
    }
}
