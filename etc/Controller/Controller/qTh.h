#ifndef QTH_H
#define QTH_H

#include <QThread>

#include "zmq_class.h"

class Controller;

class qTh : public QThread
{
    Q_OBJECT
public:
    explicit qTh(QObject* parent = nullptr);

private:
    void run();
signals:
    void setValue(ZmqData zmq_data);
    void request(ZmqData zmq_data);
};

#endif // QTH_H
