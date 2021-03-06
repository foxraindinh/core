<?php

/*
	Copyright (C) 2005-2008 Bill Marquette
	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright notice,
	this list of conditions and the following disclaimer.

	2. Redistributions in binary form must reproduce the above copyright
	notice, this list of conditions and the following disclaimer in the
	documentation and/or other materials provided with the distribution.

	THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
	INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
	AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
	AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
	OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

function subnetv4_expand($subnet) {
	$result = array();
	list ($ip, $bits) = explode("/", $subnet);
	$net  = ip2long($ip);
	$mask = (0xffffffff << (32 - $bits));
	$net &= $mask;
	$size = round(exp(log(2) * (32 - $bits)));
	for ($i = 0; $i < $size; $i += 1) {
		$result[] = long2ip($net | $i);
	}
	return $result;
}


/* include all configuration functions */

class Monitor {
	private $conf = array();
	function __construct($config) {
		$this->conf = $config;
	}

	public function p() {
		return "check {$this->get('proto')}";
	}
	private function get($var) {
		return isset($this->$var) ? $this->$var : "";
	}
	protected function config($element) {
		return isset($this->conf[$element]) ? $this->conf[$element] : "";
	}
}

class TCPMonitor extends Monitor {
	protected $proto = 'tcp';
}

class SSLMonitor extends Monitor {
	protected $proto = 'ssl';
}

class ICMPMonitor extends Monitor {
	protected $proto = 'icmp';
}

class HTTPMonitor extends Monitor {
	protected $proto = 'http';
	function __construct($config) {
		parent::__construct($config);
	}
	public function p() {
		$method = ($this->code() != "") ? $this->code() : $this->digest();
		return "check {$this->proto} {$this->path()} {$this->host()} {$method}";
	}

	private function path() {
		return $this->config('path') != "" ? "'{$this->config('path')}'" : "";
	}

	private function host() {
		return $this->config('host') != "" ? "host {$this->config('host')}" : "";
	}

	private function code() {
		return $this->config('code') != "" ? "code {$this->config('code')}" : "";
	}

	private function digest() {
		return $this->config('digest') != "" ? "digest {$this->config('digest')}" : "";
	}
}

class HTTPSMonitor extends HTTPMonitor {
	protected $proto = 'https';
}

class SendMonitor extends Monitor {
	private $proto = 'send';
	function __construct($config) {
		parent::__construct($config);
	}
	public function p() {
		return "check {$this->proto} {$this->data()} expect {$this->pattern()} {$this->ssl()}";
	}


	private function data() {
		return $this->config('send') != "" ? "\"{$this->config('send')}\"" : "\"\"";
	}

	private function pattern() {
		return $this->config('expect') != "" ? "\"{$this->config('expect')}\"" : "\"\"";
	}

	private function ssl() {
		return $this->config('ssl') == true ? "ssl" : "";
	}
}


function relayd_configure($kill_first=false) {
	global $config, $g;

	if (isset($config['load_balancer']['virtual_server'])) {
		$vs_a = $config['load_balancer']['virtual_server'];
	} else {
		$vs_a = null;
	}
	if (isset($config['load_balancer']['lbpool'])) {
		$pool_a = $config['load_balancer']['lbpool'];
	} else {
		$pool_a = null;
	}
	if (isset($config['load_balancer']['lbprotocol'])) {
		$protocol_a = $config['load_balancer']['lbprotocol'];
	} else {
		$protocol_a = null;
	}
	if (isset($config['load_balancer']['setting'])) {
		$setting = $config['load_balancer']['setting'];
	} else {
		$setting = null;
	}

	$check_a = array();

	foreach ((array)$config['load_balancer']['monitor_type'] as $type) {
		switch($type['type']) {
		case 'icmp':
			$mon = new ICMPMonitor($type['options']);
			break;
		case 'tcp':
			$mon = new TCPMonitor($type['options']);
			break;
		case 'http':
			$mon = new HTTPMonitor($type['options']);
			break;
		case 'https':
			$mon = new HTTPSMonitor($type['options']);
			break;
		case 'send':
			$mon = new SendMonitor($type['options']);
			break;
		}
		if($mon) {
			$check_a[$type['name']] = $mon->p();
		}
	}

	$fd = fopen('/var/etc/relayd.conf', 'w');
	$conf = "log updates \n";

	/* Global timeout, interval and prefork settings
	   if not specified by the user:
	   - use a 1000 ms timeout value as in pfsense 2.0.1 and above
	   - leave interval and prefork empty, relayd will use its default values */

	if (isset($setting['timeout']) && !empty($setting['timeout'])) {
		$conf .= "timeout ".$setting['timeout']." \n";
	} else {
		$conf .= "timeout 1000 \n";
	}

	if (isset($setting['interval']) && !empty($setting['interval'])) {
		$conf .= "interval ".$setting['interval']." \n";
	}

	if (isset($setting['prefork']) && !empty($setting['prefork'])) {
		$conf .= "prefork ".$setting['prefork']." \n";
	}

	/* reindex pools by name as we loop through the pools array */
	$pools = array();
	/* Virtual server pools */
	if(is_array($pool_a)) {
		for ($i = 0; isset($pool_a[$i]); $i++) {
			if(is_array($pool_a[$i]['servers'])) {
				if (!empty($pool_a[$i]['retry'])) {
					$retrytext = " retry {$pool_a[$i]['retry']}";
				} else {
					$retrytext = "";
				}
				$conf .= "table <{$pool_a[$i]['name']}> {\n";
				foreach ($pool_a[$i]['servers'] as $server) {
					if (is_subnetv4($server)) {
						foreach (subnetv4_expand($server) as $ip) {
							$conf .= "\t{$ip}{$retrytext}\n";
						}
					}
					else {
						$conf .= "\t{$server}{$retrytext}\n";
					}
				}
				$conf .= "}\n";
				/* Index by name for easier fetching when we loop through the virtual servers */
				$pools[$pool_a[$i]['name']] = $pool_a[$i];
			}
		}
	}

	$conf .= "dns protocol \"dnsproto\" {\n";
	$conf .= "\t" . "tcp { nodelay, sack, socket buffer 1024, backlog 1000 }\n";
	$conf .= "}\n";

	if(is_array($vs_a)) {
		for ($i = 0; isset($vs_a[$i]); $i++) {

			$append_port_to_name = false;
			if (is_alias($pools[$vs_a[$i]['poolname']]['port'])) {
				$dest_port_array = filter_expand_alias_array($pools[$vs_a[$i]['poolname']]['port']);
				$append_port_to_name = true;
			}
			else {
				$dest_port_array = array($pools[$vs_a[$i]['poolname']]['port']);
			}
			if (is_alias($vs_a[$i]['port'])) {
				$src_port_array = filter_expand_alias_array($vs_a[$i]['port']);
				$append_port_to_name = true;
			}
			else if ($vs_a[$i]['port']) {
				$src_port_array = array($vs_a[$i]['port']);
			}
			else {
				$src_port_array = $dest_port_array;
			}

			$append_ip_to_name = false;
			if (is_alias($vs_a[$i]['ipaddr'])) {
				$ip_list = array();
				foreach (filter_expand_alias_array($vs_a[$i]['ipaddr']) as $item) {
					log_error("item is $item");
					if (is_subnetv4($item)) {
						$ip_list = array_merge($ip_list, subnetv4_expand($item));
					}
					else {
						$ip_list[] = $item;
					}
				}
				$append_ip_to_name = true;
			}
			else if (is_subnetv4($vs_a[$i]['ipaddr'])) {
				$ip_list = subnetv4_expand($vs_a[$i]['ipaddr']);
				$append_ip_to_name = true;
			}
			else {
				$ip_list = array($vs_a[$i]['ipaddr']);
			}

			for ($j = 0; $j < count($ip_list); $j += 1) {
				$ip = $ip_list[$j];
				for ($k = 0; $k < count($src_port_array) && $k < count($dest_port_array); $k += 1) {
					$src_port  = $src_port_array[$k];
					$dest_port = $dest_port_array[$k];

					$name = $vs_a[$i]['name'];
					if ($append_ip_to_name) {
						$name .= "_" . $j;
					}
					if ($append_port_to_name) {
						$name .= "_" . $src_port;
					}

					if (($vs_a[$i]['mode'] == 'relay') || ($vs_a[$i]['relay_protocol'] == 'dns')) {
						$conf .= "relay \"{$name}\" {\n";
						$conf .= "  listen on {$ip} port {$src_port}\n";

						if ($vs_a[$i]['relay_protocol'] == "dns") {
							$conf .= "  protocol \"dnsproto\"\n";
						} else {
							$conf .= "  protocol \"{$vs_a[$i]['relay_protocol']}\"\n";
						}
						$lbmode = "";
						if ( $pools[$vs_a[$i]['poolname']]['mode'] == "loadbalance" ) {
							$lbmode = "mode loadbalance";
						}

						$conf .= "  forward to <{$vs_a[$i]['poolname']}> port {$dest_port} {$lbmode} {$check_a[$pools[$vs_a[$i]['poolname']]['monitor']]} \n";

						if (isset($vs_a[$i]['sitedown']) &&  strlen($vs_a[$i]['sitedown']) > 0 && ($vs_a[$i]['relay_protocol'] != 'dns'))
							$conf .= "  forward to <{$vs_a[$i]['sitedown']}> port {$dest_port} {$lbmode} {$check_a[$pools[$vs_a[$i]['poolname']]['monitor']]} \n";
						$conf .= "}\n";
					} else  {
						$conf .= "redirect \"{$name}\" {\n";
						$conf .= "  listen on {$ip} port {$src_port}\n";
						$conf .= "  forward to <{$vs_a[$i]['poolname']}> port {$dest_port} {$check_a[$pools[$vs_a[$i]['poolname']]['monitor']]} \n";

						if (isset($config['system']['lb_use_sticky']))
							$conf .= "  sticky-address\n";

						/* sitedown MUST use the same port as the primary pool - sucks, but it's a relayd thing */
						if (isset($vs_a[$i]['sitedown']) && strlen($vs_a[$i]['sitedown']) > 0 && ($vs_a[$i]['relay_protocol'] != 'dns'))
							$conf .= "  forward to <{$vs_a[$i]['sitedown']}> port {$dest_port} {$check_a[$pools[$vs_a[$i]['sitedown']]['monitor']]} \n";

						$conf .= "}\n";
					}
				}
			}
		}
	}
	fwrite($fd, $conf);
	fclose($fd);

	if (is_process_running('relayd')) {
		if (! empty($vs_a)) {
			if ($kill_first) {
				killbyname('relayd');
				/* Remove all active relayd anchors now that relayd is no longer running. */
				cleanup_lb_anchor('*');
				mwexec('/usr/local/sbin/relayd -f /var/etc/relayd.conf');
			} else {
				// it's running and there is a config, just reload
				mwexec('/usr/local/sbin/relayctl reload');
			}
		} else {
			/*
			 * XXX: Something breaks our control connection with relayd
			 * and makes 'relayctl stop' not work
			 * rule reloads are the current suspect
			 * mwexec('/usr/local/sbin/relayctl stop');
			 *  returns "command failed"
			 */
			killbyname('relayd');
			/* Remove all active relayd anchors now that relayd is no longer running. */
			cleanup_lb_anchor("*");
		}
	} else {
		if (! empty($vs_a)) {
			// not running and there is a config, start it
			/* Remove all active relayd anchors so it can start fresh. */
			cleanup_lb_anchor('*');
			mwexec('/usr/local/sbin/relayd -f /var/etc/relayd.conf');
		}
	}
}

function get_lb_redirects() {
/*
# relayctl show summary
Id   Type      Name                      Avlblty Status
1    redirect  testvs2                           active
5    table     test2:80                          active (3 hosts up)
11   host      192.168.1.2               91.55%  up
10   host      192.168.1.3               100.00% up
9    host      192.168.1.4               88.73%  up
3    table     test:80                           active (1 hosts up)
7    host      192.168.1.2               66.20%  down
6    host      192.168.1.3               97.18%  up
0    redirect  testvs                            active
3    table     test:80                           active (1 hosts up)
7    host      192.168.1.2               66.20%  down
6    host      192.168.1.3               97.18%  up
4    table     testvs-sitedown:80                active (1 hosts up)
8    host      192.168.1.4               84.51%  up
# relayctl show redirects
Id   Type      Name                      Avlblty Status
1    redirect  testvs2                           active
0    redirect  testvs                            active
# relayctl show redirects
Id   Type      Name                      Avlblty Status
1    redirect  testvs2                           active
           total: 2 sessions
           last: 2/60s 2/h 2/d sessions
           average: 1/60s 0/h 0/d sessions
0    redirect  testvs                            active
*/
	$rdr_a = array();
	exec('/usr/local/sbin/relayctl show redirects 2>&1', $rdr_a);
	$relay_a = array();
	exec('/usr/local/sbin/relayctl show relays 2>&1', $relay_a);
	$vs = array();
	$cur_entry = "";
	for ($i = 0; isset($rdr_a[$i]); $i++) {
		$line = $rdr_a[$i];
		if (preg_match("/^[0-9]+/", $line)) {
			$regs = array();
			if($x = preg_match("/^[0-9]+\s+redirect\s+([^\s]+)\s+([^\s]+)/", $line, $regs)) {
				$cur_entry = trim($regs[1]);
				$vs[trim($regs[1])] = array();
				$vs[trim($regs[1])]['status'] = trim($regs[2]);
			}
		} elseif (($x = preg_match("/^\s+total:\s(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['total'] = trim($regs[1]);
		} elseif (($x = preg_match("/^\s+last:\s(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['last'] = trim($regs[1]);
		} elseif (($x = preg_match("/^\s+average:(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['average'] = trim($regs[1]);
		}
	}
	$cur_entry = "";
	for ($i = 0; isset($relay_a[$i]); $i++) {
		$line = $relay_a[$i];
		if (preg_match("/^[0-9]+/", $line)) {
			$regs = array();
			if($x = preg_match("/^[0-9]+\s+relay\s+([^\s]+)\s+([^\s]+)/", $line, $regs)) {
				$cur_entry = trim($regs[1]);
				$vs[trim($regs[1])] = array();
				$vs[trim($regs[1])]['status'] = trim($regs[2]);
			}
		} elseif (($x = preg_match("/^\s+total:\s(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['total'] = trim($regs[1]);
		} elseif (($x = preg_match("/^\s+last:\s(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['last'] = trim($regs[1]);
		} elseif (($x = preg_match("/^\s+average:(.*)\ssessions/", $line, $regs)) && !empty($cur_entry)) {
			$vs[$cur_entry]['average'] = trim($regs[1]);
		}
	}
	return $vs;
}

function get_lb_summary() {
	$relayctl = array();
	exec('/usr/local/sbin/relayctl show summary 2>&1', $relayctl);
	$relay_hosts=Array();
	foreach( (array) $relayctl as $line) {
		$t = explode("\t", $line);
		if (isset($t[1])) {
			switch (trim($t[1])) {
				case "table":
					$curpool=trim($t[2]);
					break;
				case "host":
					$curhost=trim($t[2]);
					if (!isset($relay_hosts[$curpool])) {
						$relay_hosts[$curpool] = array();
					}
					if (!isset($relay_hosts[$curpool][$curhost])) {
						$relay_hosts[$curpool][$curhost]['avail'] = array();
					}
					$relay_hosts[$curpool][$curhost]['avail']=trim($t[3]);
					$relay_hosts[$curpool][$curhost]['state']=trim($t[4]);
					break;
			}
		}
	}
	return $relay_hosts;
}

/* Get a list of all relayd virtual server anchors */
function get_lb_anchors() {
	/* NOTE: These names come back prepended with "relayd/" e.g. "relayd/MyVSName" */
	return explode("\n", trim(`/sbin/pfctl -sA -a relayd | /usr/bin/awk '{print $1;}'`));
}

/* Remove NAT rules from a relayd anchor that is no longer in use.
	$anchorname can either be * to clear all anchors or a specific anchor name.*/
function cleanup_lb_anchor($anchorname = "*") {
	$lbanchors = get_lb_anchors();
	foreach ($lbanchors as $lba) {
		if (($anchorname == "*") || ($lba == "relayd/{$anchorname}")) {
			/* Flush both the NAT and the Table for the anchor, so it will be completely removed by pf. */
			mwexec("/sbin/pfctl -a " . escapeshellarg($lba) . " -F nat");
			mwexec("/sbin/pfctl -a " . escapeshellarg($lba) . " -F Tables");
		}
	}
}

/* Mark an anchor for later cleanup. This will allow us to remove an old VS name */
function cleanup_lb_mark_anchor($name)
{
	/* Nothing to do! */
	if (empty($name)) {
		return;
	}

	$filename = '/tmp/relayd_anchors_remove';
	$cleanup_anchors = array();

	/* Read in any currently unapplied name changes */
	if (file_exists($filename)) {
		$cleanup_anchors = explode("\n", file_get_contents($filename));
	}

	/* Only add the anchor to the list if it's not already there. */
	if (!in_array($name, $cleanup_anchors)) {
		$cleanup_anchors[] = $name;
	}

	file_put_contents($filename, implode("\n", $cleanup_anchors));
}
