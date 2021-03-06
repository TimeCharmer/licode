#include "rtp/StatsHandler.h"
#include "./MediaDefinitions.h"
#include "./WebRtcConnection.h"

namespace erizo {

DEFINE_LOGGER(StatsCalculator, "rtp.StatsCalculator");
DEFINE_LOGGER(IncomingStatsHandler, "rtp.IncomingStatsHandler");
DEFINE_LOGGER(OutgoingStatsHandler, "rtp.OutgoingStatsHandler");

void StatsCalculator::update(WebRtcConnection *connection, std::shared_ptr<Stats> stats) {
  if (!connection_) {
    connection_ = connection;
    stats_ = stats;
    if (!getStatsInfo().hasChild("total")) {
      getStatsInfo()["total"].insertStat("bitrateCalculated", RateStat{kBitrateStatsPeriod, 8.});
    }
  }
}

void StatsCalculator::processPacket(std::shared_ptr<dataPacket> packet) {
  RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (packet->data);
  if (chead->isRtcp()) {
    processRtcpPacket(packet);
  } else {
    processRtpPacket(packet);
  }
}

void StatsCalculator::processRtpPacket(std::shared_ptr<dataPacket> packet) {
  char* buf = packet->data;
  int len = packet->length;
  RtpHeader* head = reinterpret_cast<RtpHeader*>(buf);
  uint32_t ssrc = head->getSSRC();
  if (!connection_->isSinkSSRC(ssrc) && !connection_->isSourceSSRC(ssrc)) {
    ELOG_DEBUG("message: Unknown SSRC in processRtpPacket, ssrc: %u, PT: %u", ssrc, head->getPayloadType());
    return;
  }
  if (!getStatsInfo()[ssrc].hasChild("bitrateCalculated")) {
    if (ssrc == connection_->getVideoSourceSSRC() || ssrc == connection_->getVideoSinkSSRC()) {
      getStatsInfo()[ssrc].insertStat("type", StringStat{"video"});
    } else if (ssrc == connection_->getAudioSourceSSRC() || ssrc == connection_->getAudioSinkSSRC()) {
      getStatsInfo()[ssrc].insertStat("type", StringStat{"audio"});
    }
    getStatsInfo()[ssrc].insertStat("bitrateCalculated", RateStat{kBitrateStatsPeriod, 8.});
  }
  getStatsInfo()[ssrc]["bitrateCalculated"] += len;
  getStatsInfo()["total"]["bitrateCalculated"] += len;
}

void StatsCalculator::incrStat(uint32_t ssrc, std::string stat) {
  if (!getStatsInfo()[ssrc].hasChild(stat)) {
    getStatsInfo()[ssrc].insertStat(stat, CumulativeStat{1});
    return;
  }
  getStatsInfo()[ssrc][stat]++;
}

void StatsCalculator::processRtcpPacket(std::shared_ptr<dataPacket> packet) {
  char* buf = packet->data;
  int len = packet->length;

  char* movingBuf = buf;
  int rtcpLength = 0;
  int totalLength = 0;
  uint32_t ssrc = 0;

  RtcpHeader *chead = reinterpret_cast<RtcpHeader*>(movingBuf);
  if (chead->isFeedback()) {
    ssrc = chead->getSourceSSRC();
    if (!connection_->isSinkSSRC(ssrc)) {
      return;
    }
  } else {
    ssrc = chead->getSSRC();
        if (!connection_->isSourceSSRC(ssrc)) {
          return;
        }
  }

  ELOG_DEBUG("RTCP packet received, type: %u, size: %u, packetLength: %u", chead->getPacketType(),
       ((ntohs(chead->length) + 1) * 4), len);
  do {
    movingBuf += rtcpLength;
    RtcpHeader *chead = reinterpret_cast<RtcpHeader*>(movingBuf);
    rtcpLength = (ntohs(chead->length) + 1) * 4;
    totalLength += rtcpLength;
    ELOG_DEBUG("RTCP SubPacket: PT %d, SSRC %u, sourceSSRC %u, block count %d",
        chead->packettype, chead->getSSRC(), chead->getSourceSSRC(), chead->getBlockCount());
    switch (chead->packettype) {
      case RTCP_SDES_PT:
        ELOG_DEBUG("SDES");
        break;
      case RTCP_BYE:
        ELOG_DEBUG("RTCP BYE");
        break;
      case RTCP_Receiver_PT:
        ELOG_DEBUG("RTP RR: Fraction Lost %u, packetsLost %u", chead->getFractionLost(), chead->getLostPackets());
        getStatsInfo()[ssrc].insertStat("fractionLost", CumulativeStat{chead->getFractionLost()});
        getStatsInfo()[ssrc].insertStat("packetsLost", CumulativeStat{chead->getLostPackets()});
        getStatsInfo()[ssrc].insertStat("jitter", CumulativeStat{chead->getJitter()});
        getStatsInfo()[ssrc].insertStat("sourceSsrc", CumulativeStat{ssrc});
        break;
      case RTCP_Sender_PT:
        ELOG_DEBUG("RTP SR: Packets Sent %u, Octets Sent %u", chead->getPacketsSent(), chead->getOctetsSent());
        getStatsInfo()[ssrc].insertStat("packetsSent", CumulativeStat{chead->getPacketsSent()});
        getStatsInfo()[ssrc].insertStat("bytesSent", CumulativeStat{chead->getOctetsSent()});
        break;
      case RTCP_RTP_Feedback_PT:
        ELOG_DEBUG("RTP FB: Usually NACKs: %u", chead->getBlockCount());
        ELOG_DEBUG("PID %u BLP %u", chead->getNackPid(), chead->getNackBlp());
        incrStat(ssrc, "NACK");
        break;
      case RTCP_PS_Feedback_PT:
        ELOG_DEBUG("RTCP PS FB TYPE: %u", chead->getBlockCount() );
        switch (chead->getBlockCount()) {
          case RTCP_PLI_FMT:
            ELOG_DEBUG("PLI Packet, SSRC %u, sourceSSRC %u", chead->getSSRC(), chead->getSourceSSRC());
            incrStat(ssrc, "PLI");
            break;
          case RTCP_SLI_FMT:
            ELOG_DEBUG("SLI Message");
            incrStat(ssrc, "SLI");
            break;
          case RTCP_FIR_FMT:
            ELOG_DEBUG("FIR Packet, SSRC %u, sourceSSRC %u", chead->getSSRC(), chead->getSourceSSRC());
            incrStat(ssrc, "FIR");
            break;
          case RTCP_AFB:
            {
              ELOG_DEBUG("REMB Packet, SSRC %u, sourceSSRC %u", chead->getSSRC(), chead->getSourceSSRC());
              char *uniqueId = reinterpret_cast<char*>(&chead->report.rembPacket.uniqueid);
              if (!strncmp(uniqueId, "REMB", 4)) {
                uint64_t bitrate = chead->getREMBBitRate();
                // ELOG_DEBUG("REMB Packet numSSRC %u mantissa %u exp %u, tot %lu bps",
                //             chead->getREMBNumSSRC(), chead->getBrMantis(), chead->getBrExp(), bitrate);
                getStatsInfo()[ssrc].insertStat("bandwidth", CumulativeStat{bitrate});
              } else {
                ELOG_DEBUG("Unsupported AFB Packet not REMB")
              }
              break;
            }
          default:
            ELOG_WARN("Unsupported RTCP_PS FB TYPE %u", chead->getBlockCount());
            break;
        }
        break;
      default:
        ELOG_DEBUG("Unknown RTCP Packet, %d", chead->packettype);
        break;
    }
  } while (totalLength < len);
  notifyStats();
}

IncomingStatsHandler::IncomingStatsHandler() : connection_{nullptr} {}

void IncomingStatsHandler::enable() {}

void IncomingStatsHandler::disable() {}

void IncomingStatsHandler::notifyUpdate() {
  if (connection_) {
    return;
  }
  auto pipeline = getContext()->getPipelineShared();
  update(pipeline->getService<WebRtcConnection>().get(),
             pipeline->getService<Stats>());
}

void IncomingStatsHandler::read(Context *ctx, std::shared_ptr<dataPacket> packet) {
  processPacket(packet);
  ctx->fireRead(packet);
}

OutgoingStatsHandler::OutgoingStatsHandler() : connection_{nullptr} {}

void OutgoingStatsHandler::enable() {}

void OutgoingStatsHandler::disable() {}

void OutgoingStatsHandler::notifyUpdate() {
  if (connection_) {
    return;
  }
  auto pipeline = getContext()->getPipelineShared();
  update(pipeline->getService<WebRtcConnection>().get(),
             pipeline->getService<Stats>());
}

void OutgoingStatsHandler::write(Context *ctx, std::shared_ptr<dataPacket> packet) {
  processPacket(packet);
  ctx->fireWrite(packet);
}

}  // namespace erizo
