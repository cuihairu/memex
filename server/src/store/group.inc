// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：群聊与群能力域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— T4.1 群聊 ——
  // 建群：群主（owner）自动入群；members 须全部为已建账号（含去重）。
  // 返回群号（>0）；任何成员账号不存在返回 0。topic/purpose＝群组-1 群
  // 资料（均可空＝「未填」，服务端不强拒——定案①跳过＝直接建群）。
  std::uint64_t create_group(const std::string& name,
                             const std::string& owner,
                             const std::vector<std::string>& members,
                             const std::string& topic = "",
                             const std::string& purpose = "");
  // 群详情；不存在返回 nullopt
  std::optional<GroupInfo> group_info(std::uint64_t group_id);
  // 全量群列表（运维面：组织架构树/CLI group list）
  std::vector<GroupInfo> groups_list();
  // 某账号加入的全部群
  std::vector<GroupInfo> groups_of(const std::string& account);
  // 是否群成员
  bool is_group_member(std::uint64_t group_id, const std::string& account);
  // 群成员明细（群组-2 导出「参与人」段）：account＋role＋joined_ms，
  // 按 joined_ms 升序；群不存在返回空。
  std::vector<GroupMemberRow> group_members_detail(std::uint64_t group_id);
  // 拉人（成员须为已建账号且不在群里）
  bool group_invite(std::uint64_t group_id, const std::string& account);
  // 退群（群主退群=解散：删成员表记录；群号与归档保留）
  bool group_leave(std::uint64_t group_id, const std::string& account);
  // 群公告（R24-1：群主/管理员可设；空串=清除）。每次成功设置都落一条
  // 编辑历史（谁/何时/改成了什么——清除也落，content 空串即「清除」一笔）。
  bool group_announce(std::uint64_t group_id, const std::string& account,
                      const std::string& announcement);
  // 群资料（群组-1 主题/目的）设置：权沿群公告先例（群主/管理员可设）；
  // 两字段整组覆盖＝留空即清除（空串=「未填」态，非错误）。
  bool group_set_profile(std::uint64_t group_id, const std::string& account,
                         const std::string& topic,
                         const std::string& purpose);
  // 群公告编辑历史（R24-1：倒序，新者在前；只附加不删改）
  struct AnnouncementRevision {
    std::int64_t id{0};
    std::string editor;
    std::string content; // 该次设置后的全文（空串=该次为清除）
    std::int64_t ts_ms{0};
  };
  std::vector<AnnouncementRevision> announcement_history(
      std::uint64_t group_id, int limit = 100);
  // 群成员账号列表（群不存在返回空）
  std::vector<std::string> group_members(std::uint64_t group_id);

  // —— 平台-12 群能力开关（权限模型「群能力管理员配全」，蓝图§五）——
  //     词汇表固定七项，对应既有能力面（群公告/备忘录/密码箱/群工具/
  //     群服务器工具/群文件/外网上传收件箱）。口径：未配置=现行（允许），
  //     配置为禁即拒——首段收紧步进（存量部署不破），全面 deny-by-default
  //     留后续（如实注明）。配置权=群主/管理员（调用方把守），变更落
  //     查阅台账（action=group.capability，权限模型「全程留痕」）。
  static const std::vector<std::string>& group_capability_names();
  struct GroupCapability {
    std::uint64_t gid{0};
    std::string capability;
    bool enabled{true};
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 群须存在、能力名须在词汇表；UPSERT 一行（gid+capability 唯一）
  bool group_capability_set(std::uint64_t gid, const std::string& capability,
                            bool enabled, const std::string& by,
                            std::int64_t ts_ms);
  // 生效口径：无配置行=允许；能力名不在词汇表=拒（false）
  bool group_capability_enabled(std::uint64_t gid,
                                const std::string& capability);
  // 某群的配置行（含禁与重新允的历史现值，按能力名字典序）
  std::vector<GroupCapability> group_capabilities_list(std::uint64_t gid);
