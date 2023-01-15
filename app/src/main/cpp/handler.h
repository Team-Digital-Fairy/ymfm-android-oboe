//
// Created by Alma on 2023/01/15.
//

#ifndef YMFMTHING_HANDLER_H
#define YMFMTHING_HANDLER_H

#include <oboe/Oboe.h>

class YmfmHandler {
public:
        oboe::Result open();
        oboe::Result start();
};


#endif //YMFMTHING_HANDLER_H
