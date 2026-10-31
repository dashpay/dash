#!/usr/bin/env python3
# Copyright (c) 2016-2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# This script will locally construct a merge commit for a pull request on a
# github repository, inspect it, sign it and optionally push it.

# The following temporary branches are created/overwritten and deleted:
# * pull/$PULL/base (the current master we're merging onto)
# * pull/$PULL/head (the current state of the remote pull request)
# * pull/$PULL/merge (github's merge)
# * pull/$PULL/local-merge (our merge)

# In case of a clean merge that is accepted by the user, the local branch with
# name $BRANCH is overwritten with the merged result, and optionally pushed.
import os
from sys import stdin,stdout,stderr
import argparse
import re
import hashlib
import subprocess
import sys
import json
import codecs
import unicodedata
from urllib.request import Request, urlopen
from urllib.error import HTTPError

# External tools (can be overridden using environment)
GIT = os.getenv('GIT','git')
SHELL = os.getenv('SHELL','bash')
GH = os.getenv('GH','gh')

# OS specific configuration for terminal attributes
ATTR_RESET = ''
ATTR_PR = ''
ATTR_NAME = ''
ATTR_WARN = ''
ATTR_HL = ''
COMMIT_FORMAT = '%H %s (%an)%d'
if os.name == 'posix': # if posix, assume we can use basic terminal escapes
    ATTR_RESET = '\033[0m'
    ATTR_PR = '\033[1;36m'
    ATTR_NAME = '\033[0;36m'
    ATTR_WARN = '\033[1;31m'
    ATTR_HL = '\033[95m'
    COMMIT_FORMAT = '%C(bold blue)%H%Creset %s %C(cyan)(%an)%Creset%C(green)%d%Creset'

def sanitize(s, newlines=False):
    '''
    Strip control characters (optionally except for newlines) from a string.
    This prevent text data from doing potentially confusing or harmful things
    with ANSI formatting, linefeeds bells etc.
    '''
    return ''.join(ch for ch in s if unicodedata.category(ch)[0] != "C" or (ch == '\n' and newlines))

def git_config_get(option, default=None):
    '''
    Get named configuration option from git repository.
    '''
    try:
        return subprocess.check_output([GIT,'config','--get',option]).rstrip().decode('utf-8')
    except subprocess.CalledProcessError:
        return default

def gh_cli_token():
    '''
    Get the github.com token from the GitHub CLI, if it is installed and logged in.
    '''
    try:
        token = subprocess.check_output([GH,'auth','token','--hostname','github.com'],
                stderr=subprocess.DEVNULL, timeout=10).strip().decode('utf-8')
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    return token or None

def get_response(req_url, ghtoken):
    req = Request(req_url)
    if ghtoken is not None:
        req.add_header('Authorization', 'token ' + ghtoken)
    return urlopen(req)

def sanitize_ghdata(rec):
    '''
    Sanitize comment/review record coming from github API in-place.
    This currently sanitizes the following:
    - ['title'] PR title (optional, may not have newlines)
    - ['body'] Comment body (required, may have newlines)
    It also checks rec['user']['login'] (required) to be a valid github username.

    When anything more is used, update this function!
    '''
    if 'title' in rec: # only for PRs
        rec['title'] = sanitize(rec['title'], newlines=False)
    if rec['body'] is None:
        rec['body'] = ''
    rec['body'] = sanitize(rec['body'], newlines=True)

    if rec['user'] is None: # User deleted account
        rec['user'] = {'login': '[deleted]'}
    else:
        # "Github username may only contain alphanumeric characters or hyphens'.
        # Sometimes bot have a "[bot]" suffix in the login, so we also match for that
        # Use \Z instead of $ to not match final newline only end of string.
        if not re.match(r'[a-zA-Z0-9-]+(\[bot\])?\Z', rec['user']['login'], re.DOTALL):
            raise ValueError('Github username contains invalid characters: {}'.format(sanitize(rec['user']['login'])))
    return rec

def retrieve_json(req_url, ghtoken, use_pagination=False):
    '''
    Retrieve json from github.
    Return None if an error happens.
    '''
    try:
        reader = codecs.getreader('utf-8')
        if not use_pagination:
            return sanitize_ghdata(json.load(reader(get_response(req_url, ghtoken))))

        obj = []
        page_num = 1
        while True:
            req_url_page = '{}?page={}'.format(req_url, page_num)
            result = get_response(req_url_page, ghtoken)
            obj.extend(json.load(reader(result)))

            link = result.headers.get('link', None)
            if link is not None:
                link_next = [l for l in link.split(',') if 'rel="next"' in l]
                if len(link_next) > 0:
                    page_num = int(link_next[0][link_next[0].find("page=")+5:link_next[0].find(">")])
                    continue
            break
        return [sanitize_ghdata(d) for d in obj]
    except HTTPError as e:
        error_message = e.read()
        print('Warning: unable to retrieve pull information from github: %s' % e)
        print('Detailed error: %s' % error_message)
        return None
    except Exception as e:
        print('Warning: unable to retrieve pull information from github: %s' % e)
        return None

def retrieve_pr_info(repo,pull,ghtoken):
    req_url = "https://api.github.com/repos/"+repo+"/pulls/"+pull
    return retrieve_json(req_url,ghtoken)

def retrieve_pr_comments(repo,pull,ghtoken):
    req_url = "https://api.github.com/repos/"+repo+"/issues/"+pull+"/comments"
    return retrieve_json(req_url,ghtoken,use_pagination=True)

def retrieve_pr_reviews(repo,pull,ghtoken):
    req_url = "https://api.github.com/repos/"+repo+"/pulls/"+pull+"/reviews"
    return retrieve_json(req_url,ghtoken,use_pagination=True)

def ask_prompt(text):
    print(text,end=" ",file=stderr)
    stderr.flush()
    reply = stdin.readline().rstrip()
    print("",file=stderr)
    return reply

def get_symlink_files():
    files = sorted(subprocess.check_output([GIT, 'ls-tree', '--full-tree', '-r', 'HEAD']).splitlines())
    ret = []
    for f in files:
        if (int(f.decode('utf-8').split(" ")[0], 8) & 0o170000) == 0o120000:
            ret.append(f.decode('utf-8').split("\t")[1])
    return ret

def tree_sha512sum(commit='HEAD'):
    # request metadata for entire tree, recursively
    files = []
    blob_by_name = {}
    for line in subprocess.check_output([GIT, 'ls-tree', '--full-tree', '-r', commit]).splitlines():
        name_sep = line.index(b'\t')
        metadata = line[:name_sep].split() # perms, 'blob', blobid
        if metadata[1] == b'commit':
            continue
        assert(metadata[1] == b'blob')
        name = line[name_sep+1:]
        files.append(name)
        blob_by_name[name] = metadata[2]

    files.sort()
    # open connection to git-cat-file in batch mode to request data for all blobs
    # this is much faster than launching it per file
    p = subprocess.Popen([GIT, 'cat-file', '--batch'], stdout=subprocess.PIPE, stdin=subprocess.PIPE)
    overall = hashlib.sha512()
    for f in files:
        blob = blob_by_name[f]
        # request blob
        p.stdin.write(blob + b'\n')
        p.stdin.flush()
        # read header: blob, "blob", size
        reply = p.stdout.readline().split()
        assert(reply[0] == blob and reply[1] == b'blob')
        size = int(reply[2])
        # hash the blob data
        intern = hashlib.sha512()
        ptr = 0
        while ptr < size:
            bs = min(65536, size - ptr)
            piece = p.stdout.read(bs)
            if len(piece) == bs:
                intern.update(piece)
            else:
                raise IOError('Premature EOF reading git cat-file output')
            ptr += bs
        dig = intern.hexdigest()
        assert(p.stdout.read(1) == b'\n') # ignore LF that follows blob data
        # update overall hash with file hash
        overall.update(dig.encode("utf-8"))
        overall.update("  ".encode("utf-8"))
        overall.update(f)
        overall.update("\n".encode("utf-8"))
    p.stdin.close()
    if p.wait():
        raise IOError('Non-zero return value executing git cat-file')
    return overall.hexdigest()

def get_acks_from_comments(head_commit, comments) -> dict:
    # Look for abbreviated commit id, because not everyone wants to type/paste
    # the whole thing and the chance of collisions within a PR is small enough
    head_abbrev = head_commit[0:6]
    acks = {}
    for c in comments:
        review = [
            l for l in c["body"].splitlines()
            if "ACK" in l
            and head_abbrev in l
            and not l.startswith("> ")  # omit if quoted comment
            and not l.startswith("    ")  # omit if markdown indentation
        ]
        if review and 'coderabbit' not in c['user']['login']:
            acks[c['user']['login']] = review[0]
    return acks

def make_acks_message(head_commit, acks) -> str:
    if acks:
        ack_str ='\n\nACKs for top commit:\n'.format(head_commit)
        for name, msg in acks.items():
            ack_str += '  {}:\n'.format(name)
            ack_str += '    {}\n'.format(msg)
    else:
        ack_str ='\n\nTop commit has no ACKs.\n'
    return ack_str

def print_merge_details(pull_reference, title, branch, base_branch, head_branch, acks, message):
    print('{}{}{} {} {}into {}{}'.format(ATTR_RESET+ATTR_PR,pull_reference,ATTR_RESET,title,ATTR_RESET+ATTR_PR,branch,ATTR_RESET))
    subprocess.check_call([GIT,'--no-pager','log','--graph','--topo-order','--pretty=tformat:'+COMMIT_FORMAT,base_branch+'..'+head_branch])
    if acks is not None:
        if acks:
            print('{}ACKs:{}'.format(ATTR_PR, ATTR_RESET))
            for ack_name, ack_msg in acks.items():
                print('* {} {}({}){}'.format(ack_msg, ATTR_NAME, ack_name, ATTR_RESET))
        else:
            print('{}Top commit has no ACKs!{}'.format(ATTR_WARN, ATTR_RESET))
    show_message = False
    if message is not None and '@' in message:
        print('{}Merge message contains an @!{}'.format(ATTR_WARN, ATTR_RESET))
        show_message = True
    if message is not None and '<!-' in message:
        print('{}Merge message contains an html comment!{}'.format(ATTR_WARN, ATTR_RESET))
        show_message = True
    if show_message:
        # highlight what might have tripped a warning
        message = message.replace('@', ATTR_HL + '@' + ATTR_RESET)
        message = message.replace('<!-', ATTR_HL + '<!-' + ATTR_RESET)
        print('-' * 75)
        print(message)
        print('-' * 75)

def parse_arguments():
    epilog = '''
        In addition, you can set the following git configuration variables:
        githubmerge.repository (mandatory, e.g. <owner>/<repo>),
        githubmerge.pushmirrors (default: none, comma-separated list of mirrors to push merges of the master development branch to, e.g. `git@gitlab.com:<owner>/<repo>.git,git@github.com:<owner>/<repo>.git`),
        user.signingkey (mandatory),
        user.ghtoken (default: token from `gh auth token`, if available).
        githubmerge.merge-author-email (default: Email from git config),
        githubmerge.host (default: git@github.com),
        githubmerge.branch (no default),
        githubmerge.testcmd (default: none).
    '''
    parser = argparse.ArgumentParser(description='Utility to merge, sign and push github pull requests',
            epilog=epilog)
    parser.add_argument('--repo-from', '-r', metavar='repo_from', type=str, nargs='?',
        help='The repo to fetch the pull request from. Useful for monotree repositories. Can only be specified when branch==master. (default: githubmerge.repository setting)')
    parser.add_argument('pull', metavar='PULL', type=int, nargs=1,
        help='Pull request ID to merge')
    parser.add_argument('branch', metavar='BRANCH', type=str, nargs='?',
        default=None, help='Branch to merge against (default: githubmerge.branch setting, or base branch for pull, or \'master\')')
    return parser.parse_args()

def main():
    # Extract settings from git repo
    repo = git_config_get('githubmerge.repository')
    host = git_config_get('githubmerge.host','git@github.com')
    opt_branch = git_config_get('githubmerge.branch',None)
    merge_author_email = git_config_get('githubmerge.merge-author-email',None)
    merge_author_name = git_config_get('githubmerge.merge-author-name', 'merge-script')
    testcmd = git_config_get('githubmerge.testcmd')
    ghtoken = git_config_get('user.ghtoken') or gh_cli_token()
    signingkey = git_config_get('user.signingkey')
    if repo is None:
        print("ERROR: No repository configured. Use this command to set:", file=stderr)
        print("git config githubmerge.repository <owner>/<repo>", file=stderr)
        sys.exit(1)
    if signingkey is None:
        print("ERROR: No GPG signing key set. Set one using:",file=stderr)
        print("git config --global user.signingkey <key>",file=stderr)
        sys.exit(1)

    # Extract settings from command line
    args = parse_arguments()
    repo_from = args.repo_from or repo
    is_other_fetch_repo = repo_from != repo
    pull = str(args.pull[0])

    if host.startswith(('https:','http:')):
        host_repo = host+"/"+repo+".git"
        host_repo_from = host+"/"+repo_from+".git"
    else:
        host_repo = host+":"+repo
        host_repo_from = host+":"+repo_from

    # Receive pull information from github
    info = retrieve_pr_info(repo_from,pull,ghtoken)
    if info is None:
        sys.exit(1)
    title = info['title'].strip()
    body = info['body'].strip()
    pull_reference = repo_from + '#' + pull
    # Extract forker's repo SSH URL and branch name
    forker_repo_ssh_url = info['head']['repo']['ssh_url']
    target_branch = info['head']['ref']

    # precedence order for destination branch argument:
    #   - command line argument
    #   - githubmerge.branch setting
    #   - base branch for pull (as retrieved from github)
    #   - 'master'
    branch = args.branch or opt_branch or info['base']['ref'] or 'master'

    if branch == 'master':
        push_mirrors = git_config_get('githubmerge.pushmirrors', default='').split(',')
        push_mirrors = [p for p in push_mirrors if p]  # Filter empty string
    else:
        push_mirrors = []
        if is_other_fetch_repo:
            print('ERROR: --repo-from is only supported for the master development branch')
            sys.exit(1)

    # Initialize source branches
    head_branch = 'pull/'+pull+'/head'
    base_branch = 'pull/'+pull+'/base'
    merge_branch = 'pull/'+pull+'/merge'
    local_merge_branch = 'pull/'+pull+'/local-merge'

    devnull = open(os.devnull, 'w', encoding="utf8")
    try:
        subprocess.check_call([GIT,'checkout','-q',branch])
    except subprocess.CalledProcessError:
        print(f"ERROR: Cannot check out branch {branch}.", file=stderr)
        sys.exit(3)
    try:
        subprocess.check_call([GIT,'fetch','-q',host_repo_from,'+refs/pull/'+pull+'/*:refs/heads/pull/'+pull+'/*',
                                                          '+refs/heads/'+branch+':refs/heads/'+base_branch])
    except subprocess.CalledProcessError:
        print(f"ERROR: Cannot find pull request {pull_reference} or branch {branch} on {host_repo_from}.", file=stderr)
        sys.exit(3)

    # Ask the user if they want to rebase the branch
    rebase_reply = ask_prompt("Would you like to rebase the branch? Type 'yes' to rebase or anything else to continue without rebasing.").lower()
    if rebase_reply == 'yes':
        try:
            subprocess.check_call([GIT, 'checkout', head_branch])
            # Capture the commit hash of head_branch before the rebase
            head_commit_before_rebase = subprocess.check_output([GIT, 'rev-parse', 'HEAD']).strip().decode('utf-8')
            # Identify the base commit before the rebase
            base_commit_before_rebase = subprocess.check_output([GIT, 'merge-base', base_branch, head_branch]).strip().decode('utf-8')

            # Perform the rebase
            subprocess.check_call([GIT, 'rebase', base_branch])

            # Identify the new head commit after rebase
            new_head_commit = subprocess.check_output([GIT, 'rev-parse', 'HEAD']).strip().decode('utf-8')

            # Using git range-diff to compare changes before and after the rebase
            range_diff_output = subprocess.check_output([GIT, 'range-diff', base_commit_before_rebase + '..' + head_commit_before_rebase, base_commit_before_rebase + '..' + new_head_commit], stderr=subprocess.STDOUT)

            # Check the range-diff output for significant changes
            if not range_diff_output:
                print("No significant changes detected by git range-diff.")
            else:
                print("Significant changes detected by git range-diff. Please review the output below:")
                print(range_diff_output.decode('utf-8'))
            try:
                subprocess.check_call([GIT,'log','--graph','--topo-order','--pretty=format:'+COMMIT_FORMAT])
            except Exception:
                pass
            review_reply = ask_prompt("Do you want to continue with force push? Type 'yes' to continue or anything else to abort.").lower()
            if review_reply != 'yes':
                sys.exit(5)

            # If no significant changes or user accepts changes, force push the rebased branch to the PR branch
            # subprocess.check_call([GIT, 'push', host_repo, head_branch + ':' + 'pull/' + pull + '/head'])
            try:
                subprocess.check_call([GIT, 'push', '--force', forker_repo_ssh_url, f'HEAD:refs/heads/{target_branch}'])
                print(f"Force pushed to {target_branch} on {forker_repo_ssh_url}.")
            except subprocess.CalledProcessError as e:
                print(f"Error while pushing: {str(e)}", file=stderr)
                sys.exit(1)

        except subprocess.CalledProcessError as e:
            print("ERROR: ", e.output.decode('utf-8'))
            sys.exit(4)

    subprocess.check_call([GIT,'checkout','-q',base_branch])
    subprocess.call([GIT,'branch','-q','-D',local_merge_branch], stderr=devnull)
    subprocess.check_call([GIT,'checkout','-q','-b',local_merge_branch])
    try:
        subprocess.check_call([GIT,'--no-pager','log','-q','-1','refs/heads/'+head_branch], stdout=devnull, stderr=stdout)
        head_commit = subprocess.check_output([GIT,'--no-pager','log','-1','--pretty=format:%H',head_branch]).decode('utf-8')
        assert len(head_commit) == 40
    except subprocess.CalledProcessError:
        print(f"ERROR: Cannot find head of pull request {pull_reference} on {host_repo_from}.", file=stderr)
        sys.exit(3)
    try:
        subprocess.check_call([GIT,'--no-pager','log','-q','-1','refs/heads/'+merge_branch], stdout=devnull, stderr=stdout)
    except subprocess.CalledProcessError:
        print(f"ERROR: Cannot find merge of pull request {pull_reference} on {host_repo_from}.", file=stderr)
        sys.exit(3)
    subprocess.check_call([GIT,'checkout','-q',base_branch])
    subprocess.call([GIT,'branch','-q','-D',local_merge_branch], stderr=devnull)
    subprocess.check_call([GIT,'checkout','-q','-b',local_merge_branch])

    try:
        # Go up to the repository's root.
        toplevel = subprocess.check_output([GIT,'rev-parse','--show-toplevel']).strip()
        os.chdir(toplevel)
        # Create unsigned merge commit.
        if title:
            firstline = 'Merge {}: {}'.format(pull_reference,title)
        else:
            firstline = 'Merge {}'.format(pull_reference)
        message = firstline + '\n\n'
        message += subprocess.check_output([GIT,'--no-pager','log','--no-merges','--topo-order','--pretty=format:%H %s (%an)',base_branch+'..'+head_branch]).decode('utf-8')
        message += '\n\nPull request description:\n\n  ' + body.replace('\n', '\n  ') + '\n'
        try:
            subprocess.check_call([GIT,'merge','-q','--commit','--no-edit','--no-ff','--no-gpg-sign','-m',message.encode('utf-8'),head_branch])
        except subprocess.CalledProcessError:
            print("ERROR: Cannot be merged cleanly.",file=stderr)
            subprocess.check_call([GIT,'merge','--abort'])
            sys.exit(4)
        logmsg = subprocess.check_output([GIT,'--no-pager','log','--pretty=format:%s','-n','1']).decode('utf-8')
        if logmsg.rstrip() != firstline.rstrip():
            print("ERROR: Creating merge failed (already merged?).",file=stderr)
            sys.exit(4)

        symlink_files = get_symlink_files()
        for f in symlink_files:
            print(f"ERROR: File '{f}' was a symlink")
        if len(symlink_files) > 0:
            sys.exit(4)

        # Compute SHA512 of git tree (to be able to detect changes before sign-off)
        try:
            first_sha512 = tree_sha512sum()
        except subprocess.CalledProcessError:
            print("ERROR: Unable to compute tree hash")
            sys.exit(4)

        print_merge_details(pull_reference, title, branch, base_branch, head_branch, acks=None, message=None)
        print()

        # Run test command if configured.
        if testcmd:
            if subprocess.call(testcmd,shell=True):
                print(f"ERROR: Running '{testcmd}' failed.",file=stderr)
                sys.exit(5)

            # Show the created merge.
            diff = subprocess.check_output([GIT,'diff',merge_branch+'..'+local_merge_branch])
            subprocess.check_call([GIT,'diff',base_branch+'..'+local_merge_branch])
            if diff:
                print("WARNING: merge differs from github!",file=stderr)
                reply = ask_prompt("Type 'ignore' to continue.")
                if reply.lower() == 'ignore':
                    print("Difference with github ignored.",file=stderr)
                else:
                    sys.exit(6)
        else:
            # Verify the result manually.
            print("Dropping you on a shell so you can try building/testing the merged source.",file=stderr)
            print("Run 'git diff HEAD~' to show the changes being merged.",file=stderr)
            print("Type 'exit' when done.",file=stderr)
            if os.path.isfile('/etc/debian_version'): # Show pull number on Debian default prompt
                os.putenv('debian_chroot',pull)
            subprocess.call([SHELL,'-i'])

        second_sha512 = tree_sha512sum()
        if first_sha512 != second_sha512:
            print("ERROR: Tree hash changed unexpectedly",file=stderr)
            sys.exit(8)

        # Retrieve PR comments and ACKs and add to commit message, store ACKs to print them with commit
        # description
        comments = retrieve_pr_comments(repo_from,pull,ghtoken) + retrieve_pr_reviews(repo_from,pull,ghtoken)
        if comments is None:
            print("ERROR: Could not fetch PR comments and reviews",file=stderr)
            sys.exit(1)
        acks = get_acks_from_comments(head_commit=head_commit, comments=comments)
        message += make_acks_message(head_commit=head_commit, acks=acks)
        # end message with SHA512 tree hash, then update message
        message += '\n\nTree-SHA512: ' + first_sha512
        try:
            subprocess.check_call([GIT,'commit','--amend','--no-gpg-sign','-m',message.encode('utf-8')])
        except subprocess.CalledProcessError:
            print("ERROR: Cannot update message.", file=stderr)
            sys.exit(4)

        # Sign the merge commit.
        print_merge_details(pull_reference, title, branch, base_branch, head_branch, acks, message)
        while True:
            reply = ask_prompt("Type 's' to sign off on the above merge, or 'x' to reject and exit.").lower()
            if reply == 's':
                try:
                    config = []
                    if merge_author_name:
                        config += ['-c', f'user.name={merge_author_name}']
                    if merge_author_email:
                        config += ['-c', f'user.email={merge_author_email}']
                    subprocess.check_call([GIT] + config + ['commit','-q','--gpg-sign','--amend','--no-edit','--reset-author'])
                    break
                except subprocess.CalledProcessError:
                    print("Error while signing, asking again.",file=stderr)
            elif reply == 'x':
                print("Not signing off on merge, exiting.",file=stderr)
                sys.exit(1)

        # Put the result in branch.
        subprocess.check_call([GIT,'checkout','-q',branch])
        subprocess.check_call([GIT,'reset','-q','--hard',local_merge_branch])
    finally:
        # Clean up temporary branches.
        subprocess.call([GIT,'checkout','-q',branch])
        subprocess.call([GIT,'branch','-q','-D',head_branch],stderr=devnull)
        subprocess.call([GIT,'branch','-q','-D',base_branch],stderr=devnull)
        subprocess.call([GIT,'branch','-q','-D',merge_branch],stderr=devnull)
        subprocess.call([GIT,'branch','-q','-D',local_merge_branch],stderr=devnull)

    # Push the result.
    while True:
        reply = ask_prompt("Type 'push' to push the result to {}, branch {}, or 'x' to exit without pushing.".format(', '.join([host_repo] + push_mirrors), branch)).lower()
        if reply == 'push':
            subprocess.check_call([GIT,'push',host_repo,'refs/heads/'+branch])
            for p_mirror in push_mirrors:
                subprocess.check_call([GIT,'push',p_mirror,'refs/heads/'+branch])
            break
        elif reply == 'x':
            sys.exit(1)

if __name__ == '__main__':
    main()
