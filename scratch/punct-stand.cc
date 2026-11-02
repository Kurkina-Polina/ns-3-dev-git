/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: Kurkina Polina <polinadk9@gmail.com>
 */

// This script builds a test bench that shows how a narrowband interferer reduces the throughput
// of a wide 802.11be (Wi-Fi 7) channel in the 6 GHz band.
//
// Two basic service sets (BSS) share one spectrum channel:
//   - the main BSS, an access point and a station using an 80, 160 or 320 MHz channel;
//   - the interfering BSS, another pair of nodes occupying a single 20 MHz subchannel of it.
//
// The throughput of the main BSS is measured and printed. Sweeping the index of the occupied
// subchannel shows how much capacity is lost when one subchannel of a wide channel is busy.
//
// The command-line options are:
//   --width  main BSS channel width in MHz (80, 160 or 320)
//   --subch  index of the 20 MHz subchannel carrying interference, -1 disables interference
//   --punct  subchannel indices excluded from data frames (feasibility check, see stage 3)
//   --rngRun random number generator run number, used to repeat a point with other randomness
//
// The full list of options is printed by:
// ./ns3 run "punct-stand --help"
// Additional options are available through the generic attribute configuration system.
//
// A single line is printed at the end:
// RESULT,<width>,<subch>,<punct>,<rngRun>,<throughputMbps>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-module.h"
#include "ns3/wifi-module.h"

#include <iomanip>
#include <iostream>
#include <sstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("PunctStand");

/**
 * Create one BSS made of an access point and a single station.
 *
 * Both nodes operate on the same channel and are placed at fixed positions. The caller owns
 * the containers, so that the applications can be attached to the nodes afterwards.
 *
 * @param channelNumber channel number within the 6 GHz band
 * @param width channel width in MHz
 * @param ssidStr service set identifier, which also separates the two BSSs
 * @param dataMode transmission mode used for every data frame
 * @param apPos position of the access point
 * @param staPos position of the station
 * @param apNode container that receives the created access point node
 * @param staNode container that receives the created station node
 * @param apDev container that receives the device of the access point
 * @param staDev container that receives the device of the station
 * @param spectrumChannel spectrum channel shared by every node of the scenario
 */
static void
CreateBss(uint16_t channelNumber,
          uint32_t width,
          const std::string& ssidStr,
          const std::string& dataMode,
          Vector apPos,
          Vector staPos,
          NodeContainer& apNode,
          NodeContainer& staNode,
          NetDeviceContainer& apDev,
          NetDeviceContainer& staDev,
          Ptr<MultiModelSpectrumChannel> spectrumChannel)
{
    /// Control frames use a basic OFDM rate, as required for non-HT duplicate transmissions.
    constexpr auto controlMode = "OfdmRate6Mbps";
    SpectrumWifiPhyHelper phy;
    WifiHelper wifi;
    WifiMacHelper mac;
    MobilityHelper mobility;
    const Ssid ssid = Ssid(ssidStr);
    auto positions = CreateObject<ListPositionAllocator>();

    // Channel settings are {channel number, width in MHz, band, primary 20 MHz index}.
    // The primary subchannel is always the first one, so an interferer placed on any
    // other subchannel hits a secondary one, which is the case of interest here.
    std::ostringstream chSettings;
    chSettings << "{" << channelNumber << ", " << width << ", BAND_6GHZ, 0}";

    apNode.Create(1);
    staNode.Create(1);

    phy.SetChannel(spectrumChannel);
    phy.Set("ChannelSettings", StringValue(chSettings.str()));

    // A constant rate keeps the measured throughput a function of interference only.
    // With rate control enabled, the loss would mix channel narrowing with MCS fallback.
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode",
                                 StringValue(dataMode),
                                 "ControlMode",
                                 StringValue(controlMode));

    mac.SetMultiUserScheduler("ns3::RrMultiUserScheduler", "EnableUlOfdma", BooleanValue(false));
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
    staDev = wifi.Install(phy, mac, staNode);

    // Beacon jitter is disabled so that runs differing only in the seed stay comparable.
    mac.SetType("ns3::ApWifiMac",
                "Ssid",
                SsidValue(ssid),
                "EnableBeaconJitter",
                BooleanValue(false));
    apDev = wifi.Install(phy, mac, apNode);

    // Nodes do not move: the scenario studies interference, not mobility.
    positions->Add(apPos);
    positions->Add(staPos);
    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNode);
}

/**
 * Install a saturating downlink UDP flow from an access point to a station.
 *
 * @param sender node that transmits, that is the access point
 * @param receiver node that receives, that is the station
 * @param receiverAddress IPv4 address of the receiver
 * @param load offered load, which must exceed the capacity of the channel under test
 * @param startTime time at which the flow starts
 * @return the sink application, whose received byte count gives the throughput
 */
static ApplicationContainer
InstallDownlinkFlow(Ptr<Node> sender,
                    Ptr<Node> receiver,
                    Ipv4Address receiverAddress,
                    const std::string& load,
                    double startTime,
                    double stopTime)
{
    const uint16_t port = 5000;
    const uint32_t payloadSize = 1448;

    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(receiver);
    sinkApp.Start(Seconds(0.0));

    OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(receiverAddress, port));
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    onoff.SetAttribute("DataRate", StringValue(load));
    onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
    ApplicationContainer srcApp = onoff.Install(sender);
    srcApp.Start(Seconds(startTime));
    srcApp.Stop(Seconds(stopTime));

    return sinkApp;
}

int
main(int argc, char* argv[])
{
    uint32_t rngRun = 1;  // run number of the random number generator, varied to repeat a point
    uint32_t width = 160; // widest channel that is still quick to simulate
    int subch = -1;       // index of the interfered subchannel, -1 means no interference
    std::string punct;    // subchannels excluded from data frames, empty disables the mechanism
    double simTime = 2.0; // Measurement window: long enough to average out backoff, short enough for large sweeps.
    double warmupTime = 1.0; // association, ARP and rate control settle here, outside the measured window.
    double cooldownTime = 0.5; // lets packets already in flight reach the sink before the simulation stops
    bool verbose = false;
    const double staDist = 5.0; // close enough that the fixed MCS never fails, so only interference matters
    const double interfererDist = 7.0;  // Interferer distance.
    const double nodeHeight = 1.0; // 1m above the ground, which keeps the propagation model in its valid range.
    constexpr auto ethMcs = "EhtMcs7"; // reliable at short range, yet fast enough to saturate a channel



    CommandLine cmd(__FILE__);
    cmd.AddValue("rngRun", "Random number generator run number", rngRun);
    cmd.AddValue("width", "Main BSS channel width in MHz: 80, 160 or 320", width);
    cmd.AddValue("subch", "Index of the 20 MHz subchannel carrying interference (-1: none)", subch);
    cmd.AddValue("punct", "Comma separated subchannel indices excluded from data frames", punct);
    cmd.AddValue("verbose", "Logging", verbose);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        WifiHelper::EnableLogComponents();
    }

    RngSeedManager::SetRun(rngRun);

    // force the given subchannels out of every data frame.
    // The station manager ignores channels narrower than 80 MHz, so the interfering
    // BSS, which uses 20 MHz, is left untouched by this setting.
    if (!punct.empty())
    {
        Config::SetDefault("ns3::RrMultiUserScheduler::StaticPuncturedSubchannels",
                           StringValue(punct));
    }

    // The library picks the channel number matching the requested width, which avoids
    // hard-coding the channel numbering of the 6 GHz band in this script.
    WifiPhyOperatingChannel mainChannel;
    mainChannel.SetDefault(width, WIFI_STANDARD_80211be, WIFI_PHY_BAND_6GHZ);

    // One spectrum channel is shared by both BSSs, so that the interferer is seen only
    // on the subchannel it actually occupies rather than over the whole band.
    auto spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    auto loss = CreateObject<FriisPropagationLossModel>();
    loss->SetAttribute("Frequency", DoubleValue(6e9));
    spectrumChannel->AddPropagationLossModel(loss);
    spectrumChannel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    NodeContainer mainAp;
    NodeContainer mainSta;
    NetDeviceContainer mainApDev;
    NetDeviceContainer mainStaDev;
    CreateBss(mainChannel.GetNumber(),
              width,
              "main-bss",
              ethMcs,
              Vector(0.0, 0.0, nodeHeight),
              Vector(staDist, 0.0, nodeHeight),
              mainAp,
              mainSta,
              mainApDev,
              mainStaDev,
              spectrumChannel);

    InternetStackHelper stack;
    stack.Install(mainAp);
    stack.Install(mainSta);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    address.Assign(mainApDev);
    const Ipv4InterfaceContainer mainStaIf = address.Assign(mainStaDev);
    const auto mainLoad = "1000Mbps";



    // Measured flow: it starts after the warm-up, once association has completed.
    ApplicationContainer sinkApp = InstallDownlinkFlow(mainAp.Get(0),
                                                       mainSta.Get(0),
                                                       mainStaIf.GetAddress(0),
                                                       mainLoad,
                                                       warmupTime,
                                                       simTime);

    if (subch >= 0)
    {
        // a continuous narrowband signal on one 20 MHz subchannel.
        const double subchWidthMhz = 20;
        // Slightly narrower than 20 MHz, so that all the power stays inside the subchannel.
        const double occupiedHalfWidthMhz = 9;

        const dBm_u interfTxPower{20};
        const double interStartSec = 0.5;

        // Subchannel 0 is the lowest one in frequency, the same indexing as --punct.
        const double fcMhz = mainChannel.GetFrequency() - mainChannel.GetTotalWidth() / 2 +
                             subchWidthMhz / 2 + subch * subchWidthMhz;

        BandInfo band;
        band.fc = MHzToHz(fcMhz);
        band.fl = MHzToHz(fcMhz - occupiedHalfWidthMhz);
        band.fh = MHzToHz(fcMhz + occupiedHalfWidthMhz);
        auto interfPsd = Create<SpectrumValue>(Create<SpectrumModel>(Bands{band}));
        *interfPsd = DbmToW(interfTxPower) / MHzToHz(2 * occupiedHalfWidthMhz); // W/Hz

        NodeContainer interfNode;
        interfNode.Create(1);
        MobilityHelper interfMobility;
        auto interfPos = CreateObject<ListPositionAllocator>();
        interfPos->Add(Vector(0.0, interfererDist, nodeHeight));
        interfMobility.SetPositionAllocator(interfPos);
        interfMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
        interfMobility.Install(interfNode);

        // Duty cycle 1 makes the waveforms follow each other back to back.
        WaveformGeneratorHelper waveformHelper;
        waveformHelper.SetChannel(spectrumChannel);
        waveformHelper.SetTxPowerSpectralDensity(interfPsd);
        waveformHelper.SetPhyAttribute("Period", TimeValue(MilliSeconds(1)));
        waveformHelper.SetPhyAttribute("DutyCycle", DoubleValue(1.0));
        NetDeviceContainer waveformGeneratorDevices = waveformHelper.Install(interfNode);

        // Interference starts earlier than the measured flow, so the subchannel is
        // already busy when the main BSS begins to transmit.
        Simulator::Schedule(Seconds(interStartSec),
                            &WaveformGenerator::Start,
                            waveformGeneratorDevices.Get(0)
                                ->GetObject<NonCommunicatingNetDevice>()
                                ->GetPhy()
                                ->GetObject<WaveformGenerator>());
    }

    Simulator::Stop(Seconds(warmupTime + simTime + cooldownTime));
    Simulator::Run();

    // Only the bytes received during the measurement window are counted, so the warm-up
    // and the cool-down do not bias the result.
    const auto rxBytes = DynamicCast<PacketSink>(sinkApp.Get(0))->GetTotalRx();
    const double throughputMbps = rxBytes * 8.0 / simTime / 1e6;

    Simulator::Destroy();

    std::cout << "RESULT," << width << "," << subch << "," << punct << "," << rngRun << ","
              << std::fixed << std::setprecision(3) << throughputMbps << std::endl;

    return 0;
}
