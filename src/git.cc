// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "git.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "check.h"
#include "print.h"
#include "scope_guard.h"

namespace gitstatus {

const char* GitError() {
  const git_error* err = git_error_last();
  return err && err->message ? err->message : "unknown error";
}

const char* RepoState(git_repository* repo) {
  // These names mostly match gitaction in vcs_info:
  // https://github.com/zsh-users/zsh/blob/master/Functions/VCS_Info/Backends/VCS_INFO_get_data_git.
  switch (git_repository_state(repo)) {
    case GIT_REPOSITORY_STATE_NONE:
      return "";
    case GIT_REPOSITORY_STATE_MERGE:
      return "merge";
    case GIT_REPOSITORY_STATE_REVERT:
      return "revert";
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
      return "revert-seq";
    case GIT_REPOSITORY_STATE_CHERRYPICK:
      return "cherry";
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
      return "cherry-seq";
    case GIT_REPOSITORY_STATE_BISECT:
      return "bisect";
    case GIT_REPOSITORY_STATE_REBASE:
      return "rebase";
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
      return "rebase-i";
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
      return "rebase-m";
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
      return "am";
    case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE:
      return "am/rebase";
  }
  return "action";
}

size_t CountRange(git_repository* repo, const std::string& range) {
  git_revwalk* walk = nullptr;
  VERIFY(!git_revwalk_new(&walk, repo)) << GitError();
  ON_SCOPE_EXIT(=) { git_revwalk_free(walk); };
  VERIFY(!git_revwalk_push_range(walk, range.c_str())) << GitError();
  size_t res = 0;
  while (true) {
    git_oid oid;
    switch (git_revwalk_next(&oid, walk)) {
      case 0:
        ++res;
        break;
      case GIT_ITEROVER:
        return res;
      default:
        LOG(ERROR) << "git_revwalk_next: " << range << ": " << GitError();
        throw Exception();
    }
  }
}

git_repository* OpenRepo(const std::string& dir) {
  git_repository* repo = nullptr;
  switch (git_repository_open_ext(&repo, dir.c_str(), GIT_REPOSITORY_OPEN_FROM_ENV, nullptr)) {
    case 0:
      return repo;
    case GIT_ENOTFOUND:
      return nullptr;
    default:
      LOG(ERROR) << "git_repository_open_ext: " << Print(dir) << ": " << GitError();
      throw Exception();
  }
}

size_t NumStashes(git_repository* repo) {
  size_t res = 0;
  auto* cb = +[](size_t index, const char* message, const git_oid* stash_id, void* payload) {
    ++*static_cast<size_t*>(payload);
    return 0;
  };
  VERIFY(!git_stash_foreach(repo, cb, &res)) << GitError();
  return res;
}

std::string RemoteUrl(git_repository* repo, const git_reference* ref) {
  git_buf remote_name = {};
  if (git_branch_remote_name(&remote_name, repo, git_reference_name(ref))) return "";
  ON_SCOPE_EXIT(&) { git_buf_free(&remote_name); };

  git_remote* remote = nullptr;
  switch (git_remote_lookup(&remote, repo, remote_name.ptr)) {
    case 0:
      break;
    case GIT_ENOTFOUND:
    case GIT_EINVALIDSPEC:
      return "";
    default:
      LOG(ERROR) << "git_remote_lookup: " << GitError();
      throw Exception();
  }

  std::string res = git_remote_url(remote) ?: "";
  git_remote_free(remote);
  return res;
}

git_reference* Head(git_repository* repo) {
  git_reference* symbolic = nullptr;
  switch (git_reference_lookup(&symbolic, repo, "HEAD")) {
    case 0:
      break;
    case GIT_ENOTFOUND:
      return nullptr;
    default:
      LOG(ERROR) << "git_reference_lookup: " << GitError();
      throw Exception();
  }

  git_reference* direct = nullptr;
  if (git_reference_resolve(&direct, symbolic)) {
    LOG(INFO) << "Empty git repo (no HEAD)";
    return symbolic;
  }
  git_reference_free(symbolic);
  return direct;
}

git_reference* Upstream(git_reference* local) {
  git_reference* upstream = nullptr;
  switch (git_branch_upstream(&upstream, local)) {
    case 0:
      return upstream;
    case GIT_ENOTFOUND:
      return nullptr;
    default:
      VERIFY(git_error_last()->klass == GIT_ERROR_INVALID) << "git_branch_upstream: " << GitError();
      return nullptr;
  }
}

const char* LocalBranchName(const git_reference* ref) {
  CHECK(ref);
  git_reference_t type = git_reference_type(ref);
  switch (type) {
    case GIT_REFERENCE_DIRECT: {
      return git_reference_is_branch(ref) ? git_reference_shorthand(ref) : "";
    }
    case GIT_REFERENCE_SYMBOLIC: {
      static constexpr char kHeadPrefix[] = "refs/heads/";
      const char* target = git_reference_symbolic_target(ref);
      if (!target) return "";
      size_t len = std::strlen(target);
      if (len < sizeof(kHeadPrefix)) return "";
      if (std::memcmp(target, kHeadPrefix, sizeof(kHeadPrefix) - 1)) return "";
      return target + (sizeof(kHeadPrefix) - 1);
    }
    case GIT_REFERENCE_INVALID:
    case GIT_REFERENCE_ALL:
      break;
  }
  LOG(ERROR) << "Invalid reference type: " << type;
  throw Exception();
}

Remote GetRemote(git_repository* repo, const git_reference* ref) {
  const char* branch = nullptr;
  if (git_branch_name(&branch, ref)) return {};
  git_buf remote = {};
  if (git_branch_remote_name(&remote, repo, git_reference_name(ref))) return {};
  ON_SCOPE_EXIT(&) { git_buf_free(&remote); };
  VERIFY(std::strstr(branch, remote.ptr) == branch);
  VERIFY(branch[remote.size] == '/');
  return {remote.ptr, branch + remote.size + 1};
}

}  // namespace gitstatus
