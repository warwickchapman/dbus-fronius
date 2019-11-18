#include <QTimer>
#include <QsLog.h>
#include "inverter_gateway.h"
#include "abstract_detector.h"
#include "settings.h"

static const int MaxSimultaneousRequests = 32;

InverterGateway::InverterGateway(Settings *settings, QObject *parent) :
	QObject(parent),
	mSettings(settings),
	mTimer(new QTimer(this)),
	mAutoDetect(false),
	mScanType(None)
{
	Q_ASSERT(settings != 0);
	mAddressGenerator.setNetMaskLimit(QHostAddress(0xFFFFF000));
	mTimer->setInterval(60000);
	connect(mTimer, SIGNAL(timeout()), this, SLOT(onTimer()));
}

void InverterGateway::addDetector(AbstractDetector *detector) {
	mDetectors.append(detector);
}

bool InverterGateway::autoDetect() const
{
	return mAutoDetect;
}

void InverterGateway::setAutoDetect(bool b)
{
	if (mAutoDetect == b)
		return;
	mAutoDetect = b;
	emit autoDetectChanged();
}

int InverterGateway::scanProgress() const
{
	return mAutoDetect ? mAddressGenerator.progress(mActiveHosts.count()) : 100;
}

void InverterGateway::initializeSettings()
{
	connect(mSettings, SIGNAL(portNumberChanged()), this, SLOT(onPortNumberChanged()));
	connect(mSettings, SIGNAL(ipAddressesChanged()), this, SLOT(onIpAddressesChanged()));
}

void InverterGateway::startDetection()
{
	// startDetection is called as soon as localsettings comes up. So this
	// is a good spot to start our period re-scan timer.
	mTimer->start();

	// Do a priorityScan, followed by a fullScan if not all hosts are found
	scan(TryPriority);
}

void InverterGateway::fullScan()
{
	scan(Full);
}

void InverterGateway::scan(enum ScanType scanType)
{
	mScanType = scanType;
	mDevicesFound.clear();

	// Initialise address generator with priority addresses
	QList<QHostAddress> addresses = mSettings->ipAddresses();
	foreach (QHostAddress a, mSettings->knownIpAddresses()) {
		if (!addresses.contains(a)) {
			addresses.append(a);
		}
	}

	// If priority scan and no known PV-inverters, then we're done
	if (scanType == Priority && addresses.isEmpty())
		return;

	setAutoDetect(scanType == Full);

	QLOG_TRACE() << "Starting IP scan (" << scanType << ")";
	mAddressGenerator.setPriorityAddresses(addresses);
	mAddressGenerator.setPriorityOnly(scanType != Full);
	mAddressGenerator.reset();

	while (mActiveHosts.size() < MaxSimultaneousRequests && mAddressGenerator.hasNext()) {
		QString host = mAddressGenerator.next().toString();
		QLOG_TRACE() << "Starting scan for" << host;
		scanHost(host);
	}
}

void InverterGateway::scanHost(QString hostName)
{
	HostScan *host = new HostScan(mDetectors, hostName);
	mActiveHosts.append(host);
	connect(host, SIGNAL(finished()), this, SLOT(onDetectionDone()));
	connect(host, SIGNAL(deviceFound(const DeviceInfo &)),
			this, SLOT(onInverterFound(const DeviceInfo &)));
	host->scan();
}

void InverterGateway::onInverterFound(const DeviceInfo &deviceInfo)
{
	QList<QHostAddress> addresses = mSettings->knownIpAddresses();
	QHostAddress addr(deviceInfo.hostName);
	mDevicesFound.insert(addr);
	if (!addresses.contains(addr)) {
		addresses.append(addr);
		mSettings->setKnownIpAddresses(addresses);
	}
	emit inverterFound(deviceInfo);
}

void InverterGateway::onDetectionDone()
{
	HostScan *host = static_cast<HostScan *>(sender());
	QLOG_TRACE() << "Done scanning" << host->hostName();
	mActiveHosts.removeOne(host);
	host->deleteLater();
	updateScanProgress();

	if (mScanType > None && mAddressGenerator.hasNext()) {
		// Scan the next available host
		scanHost(mAddressGenerator.next().toString());
	} else if(mActiveHosts.size() == 0) {
		// Scan is complete
		enum ScanType scanType = mScanType;
		mScanType = None;
		setAutoDetect(false);

		QLOG_INFO() << "Auto IP scan completed. Detection finished";

		// Did we get what we came for? For full and priority scans, this is it.
		// For TryPriority scans, we switch to a full scan if we're a few
		// piggies short, and if autoScan is enabled.
		if ((scanType == TryPriority) && mSettings->autoScan()) {
			// FIXME only check that we found all known ones, not all priority
			// ones. Otherwise we do a full scan whenever a manually defined one
			// is missing
			QSet<QHostAddress> addresses = QSet<QHostAddress>::fromList(
					mAddressGenerator.priorityAddresses());

			// Do a full scan if not all devices were found
			if ((addresses - mDevicesFound).size()) {
				QLOG_INFO() << "Not all devices found, starting full IP scan";
				scan(Full);
			}
		}
	}
}

void InverterGateway::onPortNumberChanged()
{
	// If the port was changed, assume that the IP addresses did not, and
	// scan the priority addresses first, then fall back to a full scan.
	scan(TryPriority);
}

void InverterGateway::onIpAddressesChanged()
{
	// If the IP addresses changed, do a priority scan. That will scan
	// the new addresses too, and avoid a full scan.
	scan(Priority);
}

void InverterGateway::onTimer()
{
	// If we are in the middle of a sweep, don't start another one.
	if (mScanType > None)
		return;
	scan(TryPriority);
}

void InverterGateway::updateScanProgress()
{
	emit scanProgressChanged();
}

HostScan::HostScan(QList<AbstractDetector *> detectors, QString hostname, QObject *parent) :
	QObject(parent),
	mDetectors(detectors),
	mHostname(hostname)
{
}

void HostScan::scan()
{
	if (mDetectors.size()) {
		DetectorReply *reply = mDetectors.takeFirst()->start(mHostname);
		connect(reply, SIGNAL(deviceFound(const DeviceInfo &)),
			this, SLOT(onDeviceFound(const DeviceInfo &)));
		connect(reply, SIGNAL(finished()), this, SLOT(continueScan()));
	} else {
		emit finished();
	}
}

void HostScan::continueScan() {
	DetectorReply *reply = static_cast<DetectorReply *>(sender());
	reply->deleteLater();
	scan(); // Try next detector
}

void HostScan::onDeviceFound(const DeviceInfo &deviceInfo)
{
	mDetectors.clear(); // Found an inverter on this host, we're done.
	emit deviceFound(deviceInfo);
}
